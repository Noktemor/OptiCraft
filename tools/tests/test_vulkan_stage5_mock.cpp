// Host-only Vulkan API simulation. It validates descriptor source ABI,
// buffer offsets, batch copying and slot lifecycle. It is NOT GPU validation.
#include "rendering/vulkan/VulkanChunkStreamGpu.h"
#include "rendering/vulkan/VulkanGpuCuller.h"
#include "rendering/vulkan/SurfaceLod.h"
#include "rendering/vulkan/MeshDispatchRuns.h"
#include "rendering/vulkan/VulkanGpuMesher.h"
#include "rendering/vulkan/VulkanMiscMesher.h"
#include "rendering/vulkan/VulkanMaterialTables.h"
#include "rendering/vulkan/VulkanBlockAtlas.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>
#include <unordered_set>
#include "rendering/vulkan/FrameVoxelDescriptors.h"
#include "rendering/vulkan/VulkanOpaqueMaterialPipeline.h"
#include <unordered_map>
using namespace OptiCraft::Vulkan;
namespace {
struct FakeMemory {std::vector<std::byte> bytes;};
struct FakeBuffer {VkDeviceSize size;FakeMemory* memory=nullptr;};
struct FakeImage {uint32_t width=0,height=0,mips=0;FakeMemory* memory=nullptr;};
size_t atlasBlitRegions=0,atlasCopyCalls=0;
size_t copies=0,fills=0,liveCopies=0;
std::unordered_set<VkBuffer> aliveBuffers;
VkDeviceSize failAllocationSize=0;
uint32_t fakeStorageLimit=128u*1024u*1024u;
uint32_t descriptorUpdates=0;
VkDescriptorSet descriptorTarget=VK_NULL_HANDLE;
std::array<uint32_t,3> descriptorBindings{};
std::array<VkBuffer,3> descriptorBuffers{};
std::array<VkDeviceSize,3> descriptorSizes{};
struct FakeDescriptorPool {uint32_t maxSets=0,capacity=0;std::vector<VkDescriptorSet> sets;};
struct FakeDescriptorSet {std::unordered_map<uint32_t,VkDescriptorBufferInfo> bindings;};
std::unordered_map<VkDescriptorSet,FakeDescriptorSet*> descriptorState;
std::vector<VkDescriptorSet> updatedSets;

struct CullDispatch {uint32_t groups,first,count,mode;};
bool watchCull=false;
struct MeshDispatch {uint32_t x,y;std::array<uint32_t,4> params;};
bool watchMesh=false;
std::array<uint32_t,4> meshParams{};
std::vector<MeshDispatch> meshDispatches;
struct IndirectMeshDispatch {VkDeviceSize offset;std::array<uint32_t,4> params;};
std::vector<IndirectMeshDispatch> indirectMeshDispatches;
struct MeshBarrier {size_t afterDispatch;VkPipelineStageFlags src,dst;VkAccessFlags sa,da;};
std::vector<MeshBarrier> meshBarriers;
std::array<uint32_t,4> cullParams{};
std::vector<CullDispatch> cullDispatches;
std::vector<std::pair<VkPipelineStageFlags,VkPipelineStageFlags>> cullBarriers;
VkBuffer watchedLive=VK_NULL_HANDLE;
FakeBuffer& B(VkBuffer buffer){return *static_cast<FakeBuffer*>(buffer);}
FakeMemory& M(VkDeviceMemory memory){return *static_cast<FakeMemory*>(memory);}
uint32_t word(VkBuffer buffer,size_t offset){
    assert(offset+4<=B(buffer).size);
    uint32_t out=0;
    std::memcpy(&out,B(buffer).memory->bytes.data()+offset,4);
    return out;
}
LiveChunkPacket packet(int x,uint64_t instance,uint64_t version,uint32_t fill) {
    LiveChunkPacket p;
    p.position={0,x,0,0};p.chunkInstance=instance;
    p.blocksVersion=version;p.meshVersion=version;
    p.blockWords.assign(kChunkBlockWords,fill);
    p.metaWords.assign(kChunkVoxels/4,fill+1);
    p.lightWords.assign(kChunkVoxels/4,fill+2);
    return p;
}
}
extern "C" {
VkResult vkCreateBuffer(VkDevice,const VkBufferCreateInfo* info,const void*,VkBuffer* result) {
    *result=new FakeBuffer{info->size};aliveBuffers.insert(*result);return VK_SUCCESS;
}
void vkGetBufferMemoryRequirements(VkDevice,VkBuffer buf,VkMemoryRequirements* req){
    req->size=(B(buf).size+63u)&~VkDeviceSize(63u);
    req->alignment=64;req->memoryTypeBits=3;
}
void vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice,VkPhysicalDeviceMemoryProperties* p){
    p->memoryTypeCount=2;
    p->memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    p->memoryTypes[1].propertyFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
}
void vkGetPhysicalDeviceProperties(VkPhysicalDevice,VkPhysicalDeviceProperties* p){
    p->limits.nonCoherentAtomSize=64;
    p->limits.maxStorageBufferRange=fakeStorageLimit;
}
VkResult vkAllocateMemory(VkDevice,const VkMemoryAllocateInfo* info,const void*,VkDeviceMemory* mem){
    if(failAllocationSize==info->allocationSize)return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    auto* state=new FakeMemory;state->bytes.resize(size_t(info->allocationSize));*mem=state;return VK_SUCCESS;
}
VkResult vkBindBufferMemory(VkDevice,VkBuffer buf,VkDeviceMemory mem,VkDeviceSize offset){
    assert(offset==0);B(buf).memory=&M(mem);return VK_SUCCESS;
}
VkResult vkMapMemory(VkDevice,VkDeviceMemory mem,VkDeviceSize,VkDeviceSize,uint32_t,void** ptr){
    *ptr=M(mem).bytes.data();return VK_SUCCESS;
}
VkResult vkFlushMappedMemoryRanges(VkDevice,uint32_t,const VkMappedMemoryRange*) {return VK_SUCCESS;}
VkResult vkInvalidateMappedMemoryRanges(VkDevice,uint32_t,const VkMappedMemoryRange*) {return VK_SUCCESS;}
void vkUnmapMemory(VkDevice,VkDeviceMemory){}
void vkDestroyBuffer(VkDevice,VkBuffer buf,const void*){assert(aliveBuffers.erase(buf)==1);delete static_cast<FakeBuffer*>(buf);}
void vkFreeMemory(VkDevice,VkDeviceMemory mem,const void*){delete static_cast<FakeMemory*>(mem);}
void vkCmdPipelineBarrier(VkCommandBuffer,VkPipelineStageFlags src,VkPipelineStageFlags dst,
    uint32_t,uint32_t n,const VkMemoryBarrier* barriers,uint32_t,const void*,uint32_t,const void*){
    if(watchMesh){assert(n==1);meshBarriers.push_back({meshDispatches.size(),src,dst,barriers[0].srcAccessMask,barriers[0].dstAccessMask});}
    if(watchCull){assert(n==1 && (barriers[0].srcAccessMask&VK_ACCESS_SHADER_WRITE_BIT));
        assert(barriers[0].dstAccessMask&VK_ACCESS_SHADER_READ_BIT);cullBarriers.emplace_back(src,dst);}
}
void vkCmdFillBuffer(VkCommandBuffer,VkBuffer buf,VkDeviceSize offset,VkDeviceSize size,uint32_t fill){
    assert(offset+size<=B(buf).size);
    for(size_t i=0;i<size;i+=4)std::memcpy(B(buf).memory->bytes.data()+offset+i,&fill,4);
    ++fills;
}
void vkCmdCopyBuffer(VkCommandBuffer,VkBuffer src,VkBuffer dst,uint32_t count,const VkBufferCopy* regions){
    for(uint32_t i=0;i<count;i++){
        auto r=regions[i];
        assert(r.srcOffset+r.size<=B(src).size && r.dstOffset+r.size<=B(dst).size);
        std::memcpy(B(dst).memory->bytes.data()+r.dstOffset,
                    B(src).memory->bytes.data()+r.srcOffset,size_t(r.size));
        ++copies;
        if(dst==watchedLive)++liveCopies;
    }
}

void vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice,VkFormat,VkFormatProperties* p){
    p->optimalTilingFeatures=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|
        VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT|
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
}
VkResult vkCreateImage(VkDevice,const VkImageCreateInfo* info,const void*,VkImage* image){
    *image=new FakeImage{info->extent.width,info->extent.height,info->mipLevels};return VK_SUCCESS;
}
void vkGetImageMemoryRequirements(VkDevice,VkImage image,VkMemoryRequirements* r){
    auto& v=*static_cast<FakeImage*>(image);
    r->size=VkDeviceSize(v.width)*v.height*8;r->alignment=64;r->memoryTypeBits=3;
}
VkResult vkBindImageMemory(VkDevice,VkImage image,VkDeviceMemory memory,VkDeviceSize offset){
    assert(offset==0);static_cast<FakeImage*>(image)->memory=&M(memory);return VK_SUCCESS;
}
VkResult vkCreateImageView(VkDevice,const VkImageViewCreateInfo* info,const void*,VkImageView* view){
    assert(info->subresourceRange.levelCount==static_cast<FakeImage*>(info->image)->mips);
    *view=new int(1);return VK_SUCCESS;
}
VkResult vkCreateSampler(VkDevice,const VkSamplerCreateInfo* info,const void*,VkSampler* sampler){
    assert(info->minFilter==VK_FILTER_NEAREST);
    assert((info->maxLod==4.0f && info->addressModeU==VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE) ||
           (info->maxLod==0.0f && info->addressModeU==VK_SAMPLER_ADDRESS_MODE_REPEAT));
    *sampler=new int(2);return VK_SUCCESS;
}
void vkDestroyImage(VkDevice,VkImage image,const void*){delete static_cast<FakeImage*>(image);}
void vkDestroyImageView(VkDevice,VkImageView view,const void*){delete static_cast<int*>(view);}
void vkDestroySampler(VkDevice,VkSampler sampler,const void*){delete static_cast<int*>(sampler);}
void vkCmdCopyBufferToImage(VkCommandBuffer,VkBuffer buffer,VkImage image,VkImageLayout layout,
                            uint32_t count,const VkBufferImageCopy* regions){
    assert(layout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && count==1);
    const auto& reg=regions[0];
    auto& target=*static_cast<FakeImage*>(image);
    assert(reg.imageExtent.width==target.width && reg.imageExtent.height==target.height);
    const auto bytes=VkDeviceSize(target.width)*target.height*4;
    std::memcpy(target.memory->bytes.data(),B(buffer).memory->bytes.data()+reg.bufferOffset,bytes);
    ++atlasCopyCalls;
}
void vkCmdBlitImage(VkCommandBuffer,VkImage source,VkImageLayout srcLayout,
                    VkImage target,VkImageLayout dstLayout,uint32_t count,
                    const VkImageBlit* regions,uint32_t filter){
    assert(source==target && srcLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    assert(dstLayout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && filter==VK_FILTER_LINEAR);
    auto& image=*static_cast<FakeImage*>(source);
    assert(count==(image.width/16)*(image.height/16));
    for(uint32_t i=0;i<count;i++){
        const auto& b=regions[i];
        const int srcSize=16>>b.srcSubresource.mipLevel;
        const int dstSize=16>>b.dstSubresource.mipLevel;
        assert(b.dstSubresource.mipLevel==b.srcSubresource.mipLevel+1);
        assert(b.srcOffsets[1].x-b.srcOffsets[0].x==srcSize);
        assert(b.srcOffsets[1].y-b.srcOffsets[0].y==srcSize);
        assert(b.dstOffsets[1].x-b.dstOffsets[0].x==dstSize);
        assert(b.dstOffsets[1].y-b.dstOffsets[0].y==dstSize);
    }
    atlasBlitRegions+=count;
}
VkResult vkCreateDescriptorPool(VkDevice,const VkDescriptorPoolCreateInfo* info,
                                const void*,VkDescriptorPool* pool){
    assert(info->poolSizeCount==1 && info->maxSets);
    *pool=new FakeDescriptorPool{info->maxSets,info->pPoolSizes[0].descriptorCount,{}};return VK_SUCCESS;
}
VkResult vkAllocateDescriptorSets(VkDevice,const VkDescriptorSetAllocateInfo* info,
                                  VkDescriptorSet* set){
    auto& pool=*static_cast<FakeDescriptorPool*>(info->descriptorPool);
    assert(pool.sets.size()+info->descriptorSetCount<=pool.maxSets);
    for(uint32_t i=0;i<info->descriptorSetCount;++i){
        auto* state=new FakeDescriptorSet;set[i]=state;pool.sets.push_back(set[i]);descriptorState.emplace(set[i],state);
    }
    return VK_SUCCESS;
}
void vkUpdateDescriptorSets(VkDevice,uint32_t count,const VkWriteDescriptorSet* writes,
                            uint32_t,const void*){
    if(count==1 && writes[0].pImageInfo){
        assert(writes[0].pImageInfo->imageLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);return;
    }
    for(uint32_t k=0;k<count;++k){
        assert(writes[k].dstSet==writes[0].dstSet && writes[k].descriptorCount==1);
        const auto& info=*static_cast<const VkDescriptorBufferInfo*>(writes[k].pBufferInfo);
        assert(aliveBuffers.contains(info.buffer) && info.offset==0 && info.range<=B(info.buffer).size);
        if(auto it=descriptorState.find(writes[k].dstSet);it!=descriptorState.end())
            it->second->bindings[writes[k].dstBinding]=info;
        if(count==3){
            assert(info.range==B(info.buffer).size);
            descriptorBindings[k]=writes[k].dstBinding;descriptorBuffers[k]=info.buffer;descriptorSizes[k]=info.range;
        }
    }
    if(count==3){++descriptorUpdates;descriptorTarget=writes[0].dstSet;updatedSets.push_back(descriptorTarget);}
}
void vkDestroyDescriptorPool(VkDevice,VkDescriptorPool pool,const void*){
    auto* p=static_cast<FakeDescriptorPool*>(pool);
    for(auto set:p->sets){assert(descriptorState.erase(set)==1);delete static_cast<FakeDescriptorSet*>(set);}
    delete p;
}
VkResult vkCreateDescriptorSetLayout(VkDevice,const VkDescriptorSetLayoutCreateInfo*,const void*,VkDescriptorSetLayout* out){*out=new int(1);return VK_SUCCESS;}
VkResult vkCreatePipelineLayout(VkDevice,const VkPipelineLayoutCreateInfo*,const void*,VkPipelineLayout* out){*out=new int(2);return VK_SUCCESS;}
VkResult vkCreateShaderModule(VkDevice,const VkShaderModuleCreateInfo*,const void*,VkShaderModule* out){*out=new int(3);return VK_SUCCESS;}
VkResult vkCreateComputePipelines(VkDevice,void*,uint32_t count,const VkComputePipelineCreateInfo*,const void*,VkPipeline* out){for(uint32_t i=0;i<count;++i)out[i]=new int(4);return VK_SUCCESS;}
VkResult vkCreateGraphicsPipelines(VkDevice,void*,uint32_t count,const VkGraphicsPipelineCreateInfo*,const void*,VkPipeline* out){for(uint32_t i=0;i<count;++i)out[i]=new int(5);return VK_SUCCESS;}
void vkDestroyShaderModule(VkDevice,VkShaderModule h,const void*){delete static_cast<int*>(h);}
void vkDestroyPipeline(VkDevice,VkPipeline h,const void*){delete static_cast<int*>(h);}
void vkDestroyPipelineLayout(VkDevice,VkPipelineLayout h,const void*){delete static_cast<int*>(h);}
void vkDestroyDescriptorSetLayout(VkDevice,VkDescriptorSetLayout h,const void*){delete static_cast<int*>(h);}
void vkCmdBindPipeline(VkCommandBuffer,uint32_t,VkPipeline){}
void vkCmdBindDescriptorSets(VkCommandBuffer,uint32_t,VkPipelineLayout,uint32_t,uint32_t,const VkDescriptorSet*,uint32_t,const uint32_t*){}
void vkCmdPushConstants(VkCommandBuffer,VkPipelineLayout,uint32_t,uint32_t offset,uint32_t bytes,const void* data){
    if(watchMesh){assert(offset==0 && bytes==16);std::memcpy(meshParams.data(),data,16);}
    if(watchCull){assert(offset==0 && bytes==128);std::memcpy(cullParams.data(),static_cast<const std::byte*>(data)+96,16);}
}
void vkCmdDispatch(VkCommandBuffer,uint32_t x,uint32_t y,uint32_t z){
    if(watchMesh){assert(z==1);meshDispatches.push_back({x,y,meshParams});}
    if(watchCull){assert(y==1 && z==1);cullDispatches.push_back({x,cullParams[0],cullParams[1],cullParams[3]});}
}
void vkCmdDispatchIndirect(VkCommandBuffer,VkBuffer args,VkDeviceSize offset){
    assert(offset%4==0 && offset+12<=B(args).size);
    assert(word(args,offset)==0 && word(args,offset+4)==1 && word(args,offset+8)==1);
    if(watchMesh)indirectMeshDispatches.push_back({offset,meshParams});
}
void vkCmdUpdateBuffer(VkCommandBuffer,VkBuffer dst,VkDeviceSize offset,VkDeviceSize size,const void* data){
    assert(offset+size<=B(dst).size);std::memcpy(B(dst).memory->bytes.data()+offset,data,size);
}

}
int main(int argc,char** argv){
    auto device=reinterpret_cast<VkDevice>(1);
    auto physical=reinterpret_cast<VkPhysicalDevice>(2);
    auto cmd=reinterpret_cast<VkCommandBuffer>(3);
    VulkanChunkStreamGpu scene;
    scene.initialize(physical,device,{4,2,8,false});
    watchedLive=scene.liveSlotIndices();
    scene.submit({packet(0,1,1,0x1234abcd),packet(1,2,1,0x5678abcd)});
    auto batch=scene.record(cmd,0);
    assert(batch.uploads.size()==2 && scene.liveSlotCount()==2);
    assert(word(scene.blocks(),0)==0x1234abcd);
    assert(word(scene.blocks(),kChunkBlockWords*4)==0x5678abcd);
    assert(word(scene.metadata(),0)==0x1234abce);
    assert(word(scene.lights(),(kChunkVoxels/4)*4)==0x5678abcf);
    assert(word(scene.liveSlotIndices(),0)==0 && word(scene.liveSlotIndices(),4)==1);
    auto* table=reinterpret_cast<const GpuChunkSlot*>(B(scene.opaqueSlots()).memory->bytes.data());
    assert(table[0].origin[0]==0 && table[1].origin[0]==32);
    assert(table[0].neighborA[1]==int32_t(kChunkBlockWords));
    assert(table[1].neighborA[0]==0);
    auto* misc=reinterpret_cast<const GpuMiscSlot*>(B(scene.miscSlots()).memory->bytes.data());
    assert(misc[0].neighborMetaA[1]==int32_t(kChunkVoxels/4));
    assert(misc[1].neighborMetaA[0]==0);
    assert(fills==2);
    assert(liveCopies==1); // first resident-set publication
    const size_t idleCopies=copies;
    assert(scene.record(cmd,1).empty() && copies==idleCopies);
    // Geometry ranges can be supplied only for the current generation.
    auto first=scene.plan().lookup({0,0,0,0}).value();
    scene.setOpaqueFaceRange(first.slot,first.generation,50,100);
    scene.setMiscQuadRange(first.slot,first.generation,100,200);
    scene.record(cmd,1);
    table=reinterpret_cast<const GpuChunkSlot*>(B(scene.opaqueSlots()).memory->bytes.data());
    misc=reinterpret_cast<const GpuMiscSlot*>(B(scene.miscSlots()).memory->bytes.data());
    assert(table[first.slot].source[1]==50 && table[first.slot].source[2]==100);
    assert(misc[first.slot].source[2]==100 && misc[first.slot].source[3]==200);
    scene.submit({packet(0,1,2,0xFFFFFFFF)});
    auto updated=scene.record(cmd,0);
    assert(updated.uploads.size()==1 && !updated.uploads[0].resetDraw);
    assert(!updated.uploads[0].newResident);
    assert(liveCopies==1); // in-place revision must not resend the full resident list
    // An in-place update keeps its published draw until a new mesh is ready.
    assert(fills==2);
    const size_t beforeLod=copies;
    auto lodPacket=packet(0,1,2,0xFFFFFFFF);lodPacket.lodRevision=1;lodPacket.meshKinds|=kSurfaceLodFlag;
    scene.submit({lodPacket});auto lodUpdate=scene.record(cmd,1);
    assert(lodUpdate.uploads.size()==1 && !lodUpdate.uploads[0].uploadVoxels);
    assert(copies==beforeLod+2); // only the opaque/misc slot tables
    assert(word(scene.blocks(),0)==0xFFFFFFFF);
    scene.evict({0,1,0,0});
    auto removed=scene.record(cmd,1);
    assert(removed.releases.size()==1 && scene.liveSlotCount()==1);
    table=reinterpret_cast<const GpuChunkSlot*>(B(scene.opaqueSlots()).memory->bytes.data());
    assert(table[0].neighborA[1]==-1 && fills==3);
    scene.submit({packet(2,3,1,0x44556677)});
    auto reused=scene.record(cmd,0);
    assert(reused.uploads.size()==1 && reused.uploads[0].slot==1);
    assert(reused.uploads[0].generation!=removed.releases[0].generation);
    assert(fills==4);
    // Stale range from old chunk must not be installed on a recycled slot.
    try {scene.setOpaqueFaceRange(1,removed.releases[0].generation,123,456);assert(false);}
    catch(const std::invalid_argument&){}
    scene.shutdown();
    // Check every expanded word, including slot transitions and neighbours.
    VulkanChunkStreamGpu compact;
    compact.initialize(physical,device,{3,2,8,false});watchedLive=compact.liveSlotIndices();
    auto u=packet(0,10,1,0);u.blockWords={0xABCD1234};u.metaWords={0x01020304};u.lightWords={0xFEDCBA98};
    auto adjacent=u;adjacent.position.x=1;adjacent.blockWords={0xDEADBEEF};
    auto checkChannel=[&](VkBuffer b,uint32_t slot,uint32_t n,const std::vector<uint32_t>& v){
        for(uint32_t i=0;i<n;++i)assert(word(b,(size_t(slot)*n+i)*4)==v[v.size()==1?0:i]);
    };
    auto checkPacket=[&](uint32_t slot,const LiveChunkPacket& v){
        checkChannel(compact.blocks(),slot,kChunkBlockWords,v.blockWords);
        checkChannel(compact.metadata(),slot,kChunkVoxels/4,v.metaWords);
        checkChannel(compact.lights(),slot,kChunkVoxels/4,v.lightWords);
    };
    compact.submit({u,adjacent});compact.record(cmd,0);checkPacket(0,u);checkPacket(1,adjacent);
    assert(compact.uploadStats().copiedBytes==0 && compact.uploadStats().filledBytes==2*131072);
    ++u.meshVersion;u.lightWords={0xFFFFFFFF};compact.submit({u});compact.record(cmd,1);checkPacket(0,u);checkPacket(1,adjacent);
    assert(compact.uploadStats().filledBytes==2*131072+32768 && compact.uploadStats().reusedBytes==98304);
    ++u.blocksVersion;u.blockWords.assign(kChunkBlockWords,0x12345678);u.blockWords.back()=42;
    compact.submit({u});compact.record(cmd,0);checkPacket(0,u);checkPacket(1,adjacent);
    assert(compact.uploadStats().copiedBytes==65536);
    ++u.blocksVersion;u.blockWords={0xABCD1234};compact.submit({u});compact.record(cmd,1);checkPacket(0,u);checkPacket(1,adjacent);
    assert(compact.uploadStats().filledBytes==2*131072+32768+65536);
    auto stats=compact.uploadStats();++u.lodRevision;u.meshKinds|=kSurfaceLodFlag;
    compact.submit({u});compact.record(cmd,0);
    assert(compact.uploadStats().copiedBytes==stats.copiedBytes && compact.uploadStats().filledBytes==stats.filledBytes && compact.uploadStats().reusedBytes==stats.reusedBytes);
    compact.evict(u.position);compact.record(cmd,1);
    u.position.x=5;u.chunkInstance=11;compact.submit({u});compact.record(cmd,0);checkPacket(0,u);checkPacket(1,adjacent);
    assert(compact.uploadStats().filledBytes==stats.filledBytes+131072);
    compact.shutdown();

    // Stage23 default ABI: page allocation is independent for each channel;
    // no expansion writes for inline words and no slot-table stride change.
    VulkanChunkStreamGpu pages;
    pages.initialize(physical,device,{3,2,8});
    assert(pages.indirectVoxelPages());
    auto a=packet(0,20,1,0),b=packet(1,21,1,0),c=packet(2,22,1,0);
    a.blockWords={0xABCD1234};a.metaWords={0xF0010203};a.lightWords={0xFFFFFFFF};
    b.blockWords.back()=77;b.metaWords={0x11223344};b.lightWords.back()=0x01020304;
    c.blockWords={0x55555555};c.metaWords.back()=0xDEADBEEF;c.lightWords={0x80000000};
    auto verify=[&](uint32_t slot,const LiveChunkPacket& p){
        const auto* ot=reinterpret_cast<const GpuChunkSlot*>(B(pages.opaqueSlots()).memory->bytes.data());
        const auto* mt=reinterpret_cast<const GpuMiscSlot*>(B(pages.miscSlots()).memory->bytes.data());
        const auto& o=ot[slot];const auto& m=mt[slot];
        assert(o.origin==m.origin && (o.origin[3]&kVoxelAddressFlag));
        assert(o.source[0]==m.source[0] && uint32_t(o.neighborB[2])==m.source[1]);
        assert(o.neighborB[3]==m.neighborBlocksB[2]);
        const std::array<uint32_t,3> addresses{o.source[0],uint32_t(o.neighborB[2]),uint32_t(o.neighborB[3])};
        const std::array<uint32_t,3> flags{kVoxelUniformBlock,kVoxelUniformMeta,kVoxelUniformLight};
        const std::array<VkBuffer,3> buffers{pages.blocks(),pages.metadata(),pages.lights()};
        const std::array<const std::vector<uint32_t>*,3> data{&p.blockWords,&p.metaWords,&p.lightWords};
        for(size_t k=0;k<3;++k){
            const bool inl=(o.origin[3]&flags[k])!=0;
            assert(inl==(data[k]->size()==1));
            if(inl)assert(addresses[k]==(*data[k])[0]);
            else for(size_t i=0;i<data[k]->size();++i)assert(word(buffers[k],(addresses[k]+i)*4)==(*data[k])[i]);
        }
    };
    const auto fillBefore=fills;
    pages.submit({a,b});pages.record(cmd,0);verify(0,a);verify(1,b);
    pages.submit({c});pages.record(cmd,1);verify(2,c);
    assert(pages.denseVoxelBytes()==131072);
    assert(pages.uploadStats().filledBytes==0 && pages.uploadStats().inlineBytes==262144);
    auto* ot=reinterpret_cast<const GpuChunkSlot*>(B(pages.opaqueSlots()).memory->bytes.data());
    assert(ot[0].neighborA[1]==1 && ot[1].neighborA[1]==2 && ot[2].neighborA[0]==1);
    assert(ot[1].source[0]==0 && uint32_t(ot[2].neighborB[2])==0); // different physical owners
    // Uniform -> dense -> uniform; a neighbour ID stays stable even when
    // its physical page changes. No page overlap with other residents.
    ++a.blocksVersion;a.blockWords.assign(kChunkBlockWords,0xABCD1234);a.blockWords[333]=42;
    pages.submit({a});pages.record(cmd,0);verify(0,a);verify(1,b);verify(2,c);
    assert(ot[0].source[0]==kChunkBlockWords && ot[1].neighborA[0]==0);
    ++b.blocksVersion;b.blockWords={0x80008000};pages.submit({b});pages.record(cmd,1);
    ++c.blocksVersion;c.blockWords.assign(kChunkBlockWords,0x55555555);c.blockWords[3]=99;
    pages.submit({c});pages.record(cmd,0);verify(0,a);verify(1,b);verify(2,c);
    assert(ot[2].source[0]==0 && ot[1].neighborA[1]==2);
    // Release/recycle and revision-only upload preserve all channel mappings.
    pages.evict(a.position);pages.record(cmd,1);a.position.x=3;a.chunkInstance=23;
    pages.submit({a});pages.record(cmd,0);verify(0,a);verify(1,b);verify(2,c);
    const auto snapshot=pages.uploadStats();++a.lodRevision;a.meshKinds|=kSurfaceLodFlag;
    pages.submit({a});auto onlyLod=pages.record(cmd,1);verify(0,a);
    assert(!onlyLod.uploads[0].uploadVoxels && pages.uploadStats().copiedBytes==snapshot.copiedBytes);
    ++a.lodRevision;a.meshKinds|=kGeometryLodFlag;
    pages.submit({a});auto coarseLod=pages.record(cmd,0);verify(0,a);
    assert(!coarseLod.uploads[0].uploadVoxels && pages.uploadStats().copiedBytes==snapshot.copiedBytes);
    assert(uint32_t(ot[0].origin[3])&kGeometryLodFlag);
    ++a.lodRevision;a.meshKinds&=~kGeometryLodFlag;
    pages.submit({a});pages.record(cmd,1);verify(0,a);
    assert(!(uint32_t(ot[0].origin[3])&kGeometryLodFlag));
    // Only two draw-reset fills for initial batch + one for c + eviction + recycle.
    assert(fills==fillBefore+5);
    pages.shutdown();
    // Exercise the exact full-capacity invariant under arbitrary transitions.
    VoxelChannelPages pool;pool.reset(64,16384);
    for(uint32_t i=0;i<64;++i)pool.set(i,false,0);
    assert(pool.densePages()==64);
    for(uint32_t i=0;i<64;i+=2)pool.set(i,true,0xFFFFFFFFu);
    assert(pool.densePages()==32);
    for(uint32_t i=0;i<64;i+=2)pool.set(i,false,0);
    std::array<bool,64> owned{};
    for(uint32_t i=0;i<64;++i){const auto e=pool.at(i);assert(e.active&&!e.uniform&&!owned[e.value/16384]);owned[e.value/16384]=true;}
    assert(pool.densePages()==64);
    std::cout<<"Stage23 independent voxel pages / inline channels PASS\n";

    // Stage24: force tiny independent physical arenas, then exercise migration
    // through both frame slots. Actual queue execution is simulated by this mock.
    VulkanChunkStreamGpu growing;
    VulkanChunkStreamGpu::Config tiny{8,2,16,true,true,{1,1,1}};
    growing.initialize(physical,device,tiny);
    watchedLive=growing.liveSlotIndices();
    assert(growing.voxelAllocatedBytes()==131072);
    auto denseA=packet(0,30,1,0x1234abcd),denseB=packet(1,31,1,0x5678abcd);
    auto denseC=packet(2,32,1,0x9876abcd),denseD=packet(3,33,1,0x7654abcd);
    auto verifyDense=[&](const LiveChunkPacket& p){
        const auto resident=growing.plan().lookup(p.position);assert(resident);
        const auto slot=resident->slot;
        const auto* table=reinterpret_cast<const GpuChunkSlot*>(B(growing.opaqueSlots()).memory->bytes.data());
        const auto& t=table[slot];
        const std::array<uint32_t,3> offsets{t.source[0],uint32_t(t.neighborB[2]),uint32_t(t.neighborB[3])};
        const std::array<const std::vector<uint32_t>*,3> data{&p.blockWords,&p.metaWords,&p.lightWords};
        for(size_t k=0;k<3;++k){
            if(data[k]->size()==1){assert((t.origin[3]&(64u<<k)) && offsets[k]==(*data[k])[0]);}
            else for(size_t i=0;i<data[k]->size();++i)assert(word(growing.voxelBuffers()[k],(offsets[k]+i)*4)==(*data[k])[i]);
        }
        return offsets;
    };
    FrameVoxelDescriptors frameSets;frameSets.sets={reinterpret_cast<VkDescriptorSet>(0xA1),reinterpret_cast<VkDescriptorSet>(0xA2)};
    auto select=[&](uint32_t frame){return frameSets.select(device,frame,growing.voxelEpoch(),{0,8,9},growing.voxelBuffers(),growing.voxelSizes());};
    growing.submit({denseA});growing.record(cmd,0);const auto originalOffsets=verifyDense(denseA);
    const auto original=growing.voxelBuffers();
    assert(select(0)==frameSets.sets[0] && descriptorUpdates==0);
    // One growth per channel on this frame; preserve offsets and old handles.
    growing.submit({denseB});growing.record(cmd,1);
    assert(growing.voxelGrowthCount()==3 && growing.voxelEpoch()==3);
    assert(verifyDense(denseA)==originalOffsets);verifyDense(denseB);
    const auto second=growing.voxelBuffers();
    for(auto handle:original)assert(aliveBuffers.contains(handle));
    assert(growing.voxelAllocatedBytes()==4*131072ull);
    assert(select(1)==frameSets.sets[1] && descriptorUpdates==1);
    assert(descriptorTarget==frameSets.sets[1] && descriptorBuffers==second && descriptorSizes==growing.voxelSizes());
    const std::array<uint32_t,3> expectedBindings{0,8,9};assert(descriptorBindings==expectedBindings);
    select(1);assert(descriptorUpdates==1);
    // Next frame grows again while the prior migration's source is alive.
    growing.submit({denseC,denseD});growing.record(cmd,0);
    assert(growing.voxelGrowthCount()==6);assert(verifyDense(denseA)==originalOffsets);
    verifyDense(denseB);verifyDense(denseC);verifyDense(denseD);
    for(auto handle:original)assert(aliveBuffers.contains(handle));
    for(auto handle:second)assert(aliveBuffers.contains(handle));
    assert(select(0)==frameSets.sets[0] && descriptorUpdates==2);
    // Completion of slot 1 retires only the original migration source.
    growing.record(cmd,1);for(auto handle:original)assert(!aliveBuffers.contains(handle));
    for(auto handle:second)assert(aliveBuffers.contains(handle));
    select(1);assert(descriptorUpdates==3);
    growing.record(cmd,0);for(auto handle:second)assert(!aliveBuffers.contains(handle));
    assert(growing.voxelAllocatedBytes()==6*131072ull);
    assert(verifyDense(denseA)==originalOffsets);verifyDense(denseD);
    growing.shutdown();

    // Allocation failure after another channel has grown: the growth still
    // migrates, the rejected packet remains queued, old resident stays valid.
    growing.initialize(physical,device,tiny);watchedLive=growing.liveSlotIndices();
    growing.submit({denseA});growing.record(cmd,0);const auto before=verifyDense(denseA);
    failAllocationSize=3*32768; // fail meta geometric growth, minimum fallback succeeds
    growing.submit({denseB});growing.record(cmd,1);failAllocationSize=0;
    verifyDense(denseB);assert(verifyDense(denseA)==before);
    assert(growing.voxelSizes()[1]==2*32768);
    growing.shutdown();
    // Block growth succeeds but metadata cannot allocate a larger buffer.
    tiny.uploadsPerFrame=1;
    growing.initialize(physical,device,tiny);watchedLive=growing.liveSlotIndices();
    growing.submit({denseA});growing.record(cmd,0);const auto firstOffsets=verifyDense(denseA);
    failAllocationSize=2*32768;
    const auto oldBlock=growing.blocks();
    growing.submit({denseB});const auto rejected=growing.record(cmd,1);
    assert(rejected.uploads.empty() && growing.plan().pendingCount()==1 && growing.liveSlotCount()==1);
    assert(growing.blocks()!=oldBlock && aliveBuffers.contains(oldBlock));
    assert(verifyDense(denseA)==firstOffsets && growing.voxelAdmissionMisses()==1);
    // Uniform candidates farther back must not be starved by a dense packet.
    auto uniformChunk=packet(2,34,1,0);uniformChunk.blockWords={7};uniformChunk.metaWords={8};uniformChunk.lightWords={9};
    uniformChunk.priority=99;growing.submit({uniformChunk});
    const auto admitted=growing.record(cmd,0);assert(admitted.uploads.size()==1 && admitted.uploads[0].packet.position==uniformChunk.position);
    verifyDense(uniformChunk);assert(growing.plan().pendingCount()==1);
    // Evicting the old resident frees the needed page despite retry cooldown.
    growing.evict(denseA.position);growing.record(cmd,1);assert(!aliveBuffers.contains(oldBlock));
    growing.record(cmd,0);verifyDense(denseB);assert(growing.plan().pendingCount()==0);
    failAllocationSize=0;growing.shutdown();
    std::cout<<"Stage24 physical voxel arena growth / migration / pressure / frame descriptor epochs PASS\n";

    // Stage25: shrink only the unoccupied tail, after idle hysteresis.
    tiny.uploadsPerFrame=2;tiny.shrinkDelayFrames=3;
    growing.initialize(physical,device,tiny);watchedLive=growing.liveSlotIndices();
    growing.submit({denseA,denseB});growing.record(cmd,0);
    growing.submit({denseC,denseD});growing.record(cmd,1);
    const auto kept=verifyDense(denseA),high=verifyDense(denseD);
    const auto fullSizes=growing.voxelSizes();
    assert(high[0]>kept[0]);
    growing.evict(denseB.position);growing.evict(denseC.position);growing.evict(denseA.position);
    growing.record(cmd,0);
    for(int i=0;i<6;++i)growing.record(cmd,uint32_t(i%2));
    // A high live page prevents trimming below its address despite low occupancy.
    assert(growing.voxelSizes()[0]>=(uint64_t(high[0])+16384)*4);
    assert(verifyDense(denseD)==high);
    // Reset to test a known low live page.
    growing.shutdown();
    growing.initialize(physical,device,tiny);watchedLive=growing.liveSlotIndices();
    growing.submit({denseA,denseB});growing.record(cmd,0);
    growing.submit({denseC,denseD});growing.record(cmd,1);
    const auto stable=verifyDense(denseA);
    growing.evict(denseB.position);growing.evict(denseC.position);growing.evict(denseD.position);
    growing.record(cmd,0);const auto large=growing.voxelBuffers();
    const auto epoch=growing.voxelEpoch();
    growing.record(cmd,1);growing.record(cmd,0);
    assert(growing.voxelShrinkCount()==0);
    growing.record(cmd,1);assert(growing.voxelShrinkCount()==3 && growing.voxelEpoch()==epoch+3);
    const auto smaller=growing.voxelSizes();assert(smaller[0]<fullSizes[0]);
    assert(verifyDense(denseA)==stable);
    for(auto handle:large)assert(aliveBuffers.contains(handle));
    growing.record(cmd,0);for(auto handle:large)assert(aliveBuffers.contains(handle));
    growing.record(cmd,1);for(auto handle:large)assert(!aliveBuffers.contains(handle));
    growing.submit({denseB,denseC});growing.record(cmd,0);
    growing.submit({denseD});growing.record(cmd,1);
    verifyDense(denseA);verifyDense(denseB);verifyDense(denseC);verifyDense(denseD);
    growing.shutdown();
    // An evicted dense slot is not the slot being updated, but its free pages
    // must still be available during admission (without allocating new buffers).
    tiny.uploadsPerFrame=2;
    growing.initialize(physical,device,tiny);watchedLive=growing.liveSlotIndices();
    growing.submit({denseA,uniformChunk});growing.record(cmd,0);
    auto replacement=packet(2,34,2,0x7654abcd);
    growing.evict(denseA.position);growing.submit({replacement});
    const auto sameFrame=growing.record(cmd,1);
    assert(sameFrame.releases.size()==1 && sameFrame.uploads.size()==1);
    assert(growing.voxelGrowthCount()==0 && growing.voxelAdmissionMisses()==0);
    verifyDense(replacement);
    growing.shutdown();
    // Shrink allocation may fail while the old arena is still needed.
    growing.initialize(physical,device,tiny);watchedLive=growing.liveSlotIndices();
    growing.submit({denseA,denseB});growing.record(cmd,0);
    growing.submit({denseC,denseD});growing.record(cmd,1);
    const auto preserved=verifyDense(denseA);
    growing.evict(denseB.position);growing.evict(denseC.position);growing.evict(denseD.position);
    growing.record(cmd,0);
    // Pending work prevents trimming even if the meshing budget blocks upload.
    growing.submit({denseB});
    for(int i=0;i<6;++i)growing.record(cmd,uint32_t(i%2),false);
    assert(growing.voxelShrinkCount()==0 && growing.plan().pendingCount()==1);
    growing.evict(denseB.position); // cancels queued, nonresident packet
    const auto oldBlocks=growing.blocks();
    failAllocationSize=growing.voxelSizes()[0]/2;
    for(int i=0;i<3;++i)growing.record(cmd,uint32_t(i%2));
    assert(growing.blocks()==oldBlocks && growing.voxelShrinkCount()==2);
    assert(verifyDense(denseA)==preserved);
    failAllocationSize=0;
    for(int i=0;i<35;++i)growing.record(cmd,uint32_t(i%2));
    assert(growing.blocks()!=oldBlocks && growing.voxelShrinkCount()>=3);
    assert(verifyDense(denseA)==preserved);
    growing.shutdown();
    std::cout<<"Stage25 idle tail shrink / retirement / regrowth / OOM / pending work / same-frame eviction credit PASS\n";

    GpuBlockCatalog catalog;
    catalog.opaqueMesherFlags.assign(65536,0);
    catalog.blocks.resize(65536);
    catalog.shapeIndex.resize(256);
    catalog.shapeBoxes.resize(1);
    catalog.faces.resize(3);
    catalog.opaqueMesherFlags[2]=3;
    catalog.blocks[2].flags=VisualOpaqueCube;
    catalog.faces[1].uv={0.25f,0.5f,0.75f,1.0f};
    VulkanMaterialTables material;
    material.initialize(physical,device,catalog);
    material.recordUpload(cmd);
    assert(word(material.opaqueFlags(),2*4)==3);
    assert(word(material.blockDescriptors(),2*sizeof(GpuBlockVisual)+12)==VisualOpaqueCube);
    try {material.recordUpload(cmd);assert(false);}catch(const std::logic_error&){}
    material.releaseStagingAfterCompletion();
    // Exercise the actual descriptor owners (2 mesh lanes, 2 frame sets
    // each, plus opaque graphics) with mocked Vulkan allocation and updates.
    GpuBlockCatalog fullCatalog=catalog;
    fullCatalog.shapeIndex.resize(33*256);
    VulkanMaterialTables fullMaterials;fullMaterials.initialize(physical,device,fullCatalog);
    VulkanChunkStreamGpu liveStream;liveStream.initialize(physical,device,{8,2,16,true,true,{1,1,1}});
    watchedLive=liveStream.liveSlotIndices();
    VulkanChunkMeshing liveMeshing;VulkanChunkMeshing::Config meshConfig;
    meshConfig.batchLimit=2;meshConfig.opaqueArenaBytes=65536;meshConfig.miscArenaBytes=65536;
    const std::array<uint32_t,1> fakeSpirv{0x07230203u};
    liveMeshing.initialize(physical,device,liveStream,fullMaterials,meshConfig,
        {fakeSpirv,fakeSpirv,fakeSpirv,fakeSpirv,fakeSpirv,fakeSpirv});
    VulkanOpaqueMaterialPipeline graphics;
    graphics.initialize(device,reinterpret_cast<VkRenderPass>(7),reinterpret_cast<VkDescriptorSetLayout>(8),
        liveStream,liveMeshing,fullMaterials,fakeSpirv,fakeSpirv);
    auto selectLive=[&](uint32_t frame){
        liveMeshing.selectVoxelFrame(frame);
        graphics.selectVoxelFrame(frame,liveStream.voxelEpoch(),liveStream.voxelBuffers(),liveStream.voxelSizes());
    };
    auto checkBindings=[&](VkDescriptorSet set,std::array<uint32_t,3> indices,const std::array<VkBuffer,3>& handles,
                           const std::array<VkDeviceSize,3>& sizes){
        const auto& bindings=descriptorState.at(set)->bindings;
        for(size_t i=0;i<3;++i){assert(bindings.at(indices[i]).buffer==handles[i] && bindings.at(indices[i]).range==sizes[i]);}
    };
    liveStream.submit({denseA});liveStream.record(cmd,0);selectLive(0);
    const std::array<VkDescriptorSet,3> set0{liveMeshing.opaqueSet(),liveMeshing.miscSet(),graphics.descriptorSet()};
    const std::array<std::array<uint32_t,3>,3> bindMap{{{0,8,9},{0,1,10},{6,4,5}}};
    const auto liveOld=liveStream.voxelBuffers();const auto liveOldSizes=liveStream.voxelSizes();
    for(size_t i=0;i<3;++i)checkBindings(set0[i],bindMap[i],liveOld,liveOldSizes);
    updatedSets.clear();liveStream.submit({denseB});liveStream.record(cmd,1);selectLive(1);
    const std::array<VkDescriptorSet,3> set1{liveMeshing.opaqueSet(),liveMeshing.miscSet(),graphics.descriptorSet()};
    assert(updatedSets.size()==5); // two lanes x two compute sets + graphics
    for(size_t i=0;i<3;++i){
        assert(set1[i]!=set0[i]);
        checkBindings(set0[i],bindMap[i],liveOld,liveOldSizes);
        checkBindings(set1[i],bindMap[i],liveStream.voxelBuffers(),liveStream.voxelSizes());
    }
    for(auto set:updatedSets)assert(std::find(set0.begin(),set0.end(),set)==set0.end());
    updatedSets.clear();selectLive(1);assert(updatedSets.empty());
    liveStream.record(cmd,0);selectLive(0);assert(updatedSets.size()==5);
    for(size_t i=0;i<3;++i)checkBindings(set0[i],bindMap[i],liveStream.voxelBuffers(),liveStream.voxelSizes());
    liveStream.record(cmd,1);for(auto handle:liveOld)assert(!aliveBuffers.contains(handle));
    // Stage28: actual mesh owner records a classification prepass only for
    // eligible jobs. Its scratch is the suffix of binding 10, not a new binding.
    {
        GpuMesherBuffers b{};
        const auto& bindings=descriptorState.at(set1[0])->bindings;
        for(uint32_t i=0;i<11;++i){b.buffers[i]=bindings.at(i).buffer;b.byteSizes[i]=bindings.at(i).range;}
        b.cacheRectsPerJob=meshConfig.opaqueCacheRects;b.coarseCache=true;
        assert(b.byteSizes[10]>=2ull*(4ull+8ull*b.cacheRectsPerJob+4ull*kCoarseWordsPerJob));
        VulkanGpuMesher m;m.initialize(physical,device,b,fakeSpirv,fakeSpirv,fakeSpirv,fakeSpirv);
        const std::array<uint8_t,2> kinds{uint8_t(1u|16u|kCoarseOpaqueMesh),1u};
        watchMesh=true;meshDispatches.clear();meshBarriers.clear();
        m.recordCount(cmd,2,kinds);
        assert(meshDispatches.size()==2);
        assert(meshDispatches[0].x==8 && meshDispatches[0].y==1 && meshDispatches[0].params[0]==0 &&
            meshDispatches[0].params[1]==(2u|kCoarseCacheFlag) && meshDispatches[0].params[3]==1);
        assert(meshDispatches[1].x==192 && meshDispatches[1].y==2 && meshDispatches[1].params[3]==0);
        assert(std::any_of(meshBarriers.begin(),meshBarriers.end(),[](const auto& r){return r.afterDispatch==1 &&
            r.src==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT && r.dst==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT &&
            (r.sa&VK_ACCESS_SHADER_WRITE_BIT) && (r.da&VK_ACCESS_SHADER_READ_BIT);}));
        meshDispatches.clear();meshBarriers.clear();
        const std::array<uint8_t,2> emitKinds{uint8_t(1u|16u|kCoarseOpaqueMesh),0u};
        m.recordEmit(cmd,2,emitKinds);
        assert(meshDispatches.size()==2 && meshDispatches[0].x==192 && meshDispatches[0].y==1 &&
            meshDispatches[0].params[1]==(2u|kCoarseCacheFlag) && meshDispatches[0].params[3]==0);
        assert(meshDispatches[1].params[1]==2u); // finalize gets real job count
        meshDispatches.clear();
        const std::array<uint8_t,2> ordinary{1u,1u};m.recordCount(cmd,2,ordinary);
        assert(meshDispatches.size()==1 && meshDispatches[0].x==192 && meshDispatches[0].params[1]==2u);
        watchMesh=false;m.shutdown();
        b.coarseCache=false;m.initialize(physical,device,b,fakeSpirv,fakeSpirv,fakeSpirv,fakeSpirv);
        watchMesh=true;meshDispatches.clear();m.recordCount(cmd,2,kinds);
        assert(meshDispatches.size()==1 && meshDispatches[0].x==192 && meshDispatches[0].params[1]==2u);
        watchMesh=false;m.shutdown();
        b.coarseCache=true;b.planeCompaction=true;
        m.initialize(physical,device,b,fakeSpirv,fakeSpirv,fakeSpirv,fakeSpirv);
        watchMesh=true;meshDispatches.clear();meshBarriers.clear();indirectMeshDispatches.clear();
        m.recordCount(cmd,2,kinds);
        assert(meshDispatches.size()==3 && meshDispatches[0].x==8 && meshDispatches[0].params[3]==1);
        assert(meshDispatches[1].x==kPlaneScanGroups && meshDispatches[1].y==1 && meshDispatches[1].params[3]==2);
        assert(meshDispatches[2].x==192 && meshDispatches[2].params[0]==1);
        assert(indirectMeshDispatches.size()==1 && indirectMeshDispatches[0].offset==planeArgsOffset(2,b.cacheRectsPerJob,0));
        assert(indirectMeshDispatches[0].params[1]==(2u|kCoarseCacheFlag|kPlaneCompactFlag));
        assert(std::any_of(meshBarriers.begin(),meshBarriers.end(),[](const auto& r){return r.afterDispatch==2 &&
            (r.src&VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) && (r.dst&VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT) &&
            (r.sa&VK_ACCESS_SHADER_WRITE_BIT) && (r.da&VK_ACCESS_INDIRECT_COMMAND_READ_BIT);}));
        meshDispatches.clear();meshBarriers.clear();indirectMeshDispatches.clear();
        m.recordEmit(cmd,2,emitKinds);
        assert(meshDispatches.size()==1 && meshDispatches[0].params[1]==2u && indirectMeshDispatches.size()==1);
        assert(std::any_of(meshBarriers.begin(),meshBarriers.end(),[](const auto& r){return
            (r.dst&VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT) && (r.da&VK_ACCESS_INDIRECT_COMMAND_READ_BIT);}));
        const std::array<uint8_t,2> cached{uint8_t(emitKinds[0]|kCachedOpaqueMesh),0u};
        meshDispatches.clear();indirectMeshDispatches.clear();m.recordEmit(cmd,2,cached);
        assert(meshDispatches.size()==2 && meshDispatches[0].x==8 && indirectMeshDispatches.empty());
        meshDispatches.clear();indirectMeshDispatches.clear();m.recordCount(cmd,2,ordinary);
        assert(meshDispatches.size()==1 && indirectMeshDispatches.empty());
        watchMesh=false;m.shutdown();
        // Live stream -> batchKinds -> mesh owner, not only a direct API probe.
        auto coarse=packet(7,42,1,0x00020002);coarse.meshKinds=1u|kLodFlags;
        liveStream.submit({coarse});auto changed=liveStream.record(cmd,0);selectLive(0);
        watchMesh=true;meshDispatches.clear();meshBarriers.clear();indirectMeshDispatches.clear();liveMeshing.record(cmd,0,changed);watchMesh=false;
        assert(std::any_of(meshDispatches.begin(),meshDispatches.end(),[](const auto& d){return d.x==8 && d.params[3]==1 && (d.params[1]&kCoarseCacheFlag); }));
        assert(!indirectMeshDispatches.empty());
        std::cout<<"Stage31 indirect Count/Emit, barrier, cache-copy and live LOD3 routing PASS (mock API)\n";
        std::cout<<"Stage28 actual Count/Emit dispatch, barrier, disabled mode and live LOD3 job routing PASS (mock API)\n";
    }
    graphics.shutdown();liveMeshing.shutdown();fullMaterials.shutdown();liveStream.shutdown();
    assert(descriptorState.empty());
    std::cout<<"Stage24 actual compute/graphics descriptor owners with two lanes / two frames PASS (mock API)\n";
    // Stage26: exercise the actual high-ID stream on the fake 128 MiB SSBO device.
    {
    VulkanChunkStreamGpu large;
    large.initialize(physical,device,{4096,64,8192,true,true,{1,1,1}});
    assert(large.plan().capacity()==4096);
    assert((large.voxelSizes()==std::array<VkDeviceSize,3>{65536,32768,32768}));
    std::vector<LiveChunkPacket> uniformPackets;
    for(int x=0;x<4096;++x){LiveChunkPacket p;p.position={0,x,0,0};p.chunkInstance=x+1;
        p.blocksVersion=p.meshVersion=1;p.priority=x;p.blockWords={0};p.metaWords={0};p.lightWords={0};uniformPackets.push_back(std::move(p));}
    large.submit(std::move(uniformPackets));
    for(uint32_t frame=0;frame<64;++frame){const auto b=large.record(cmd,frame%2);assert(b.uploads.size()==64);}
    assert(large.liveSlotCount()==4096 && large.plan().pendingCount()==0);
    assert(word(large.liveSlotIndices(),4095*4)==4095);
    const auto* largeTable=reinterpret_cast<const GpuChunkSlot*>(B(large.opaqueSlots()).memory->bytes.data());
    assert(largeTable[4095].origin[0]==4095*32 && largeTable[4095].neighborA[0]==4094);
    large.submit({packet(4095,4096,2,0x12345678)});auto high=large.record(cmd,0);
    assert(high.uploads.size()==1 && high.uploads[0].slot==4095);
    assert(word(large.blocks(),0)==0x12345678);
    // A fixed-address diagnostic path cannot allocate 4096 dense pages on this device.
    VulkanChunkStreamGpu fixed;bool rejected=false;
    try{fixed.initialize(physical,device,{4096,8,8192,false,false});}catch(const std::invalid_argument&){rejected=true;}
    assert(rejected);
    VulkanDeviceBuffer published,result,counter;
    published.create(physical,device,4096*16,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    result.create(physical,device,4096*32,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    counter.create(physical,device,4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VulkanCullerBuffers cull{};cull.depthSort=true;cull.chunkSlotStride=sizeof(GpuMiscSlot);
    cull.buffers={large.miscSlots(),published.buffer(),large.liveSlotIndices(),result.buffer(),counter.buffer()};
    cull.byteSizes={4096*sizeof(GpuMiscSlot),4096*16,4096*4,4096*32,4};
    VulkanGpuCuller sorter;const std::array<uint32_t,1> fakeSpv{0x07230203u};
    auto undersized=cull;undersized.byteSizes[3]=4096*16;rejected=false;
    try{sorter.initialize(physical,device,undersized,fakeSpv);}catch(const std::invalid_argument&){rejected=true;}
    assert(rejected);
    sorter.initialize(physical,device,cull,fakeSpv);assert(sorter.maxJobs()==4096);
    watchCull=true;
    sorter.record(cmd,2048,{});
    assert(cullDispatches.size()==1 && cullDispatches[0].groups==1 && cullDispatches[0].mode==0);
    assert(cullBarriers.size()==1);cullDispatches.clear();cullBarriers.clear();
    for(auto count:{2049u,4096u}){
        sorter.record(cmd,count,{});assert(cullDispatches.size()==2 && cullBarriers.size()==2);
        assert(cullDispatches[0].groups==2 && cullDispatches[0].mode==1 && cullDispatches[0].first==4096);
        assert(cullDispatches[1].groups==(count+63)/64 && cullDispatches[1].mode==2 && cullDispatches[1].first==4096);
        assert(cullDispatches[0].count==count && cullDispatches[1].count==count);
        assert(cullBarriers[0].first==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT && cullBarriers[0].second==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        assert(cullBarriers[1].second&VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);
        cullDispatches.clear();cullBarriers.clear();
    }
    watchCull=false;sorter.shutdown();published.destroy();result.destroy();counter.destroy();large.shutdown();
    assert(descriptorState.empty());
    // Simulate a tighter SSBO cap without allocating hundreds of MiB in the mock.
    fakeStorageLimit=512u*1024u;
    VulkanChunkStreamGpu limited;
    limited.initialize(physical,device,{4096,16,8192,true,true,{8,1,1}});
    std::vector<LiveChunkPacket> pressure;
    for(int x=0;x<9;++x){auto p=packet(x,x+1,1,0x12345678);p.metaWords={0};p.lightWords={0};pressure.push_back(std::move(p));}
    auto inlinePacket=packet(9,10,1,0);inlinePacket.blockWords={0};inlinePacket.metaWords={0};inlinePacket.lightWords={0};pressure.push_back(std::move(inlinePacket));
    limited.submit(std::move(pressure));const auto admitted=limited.record(cmd,0);
    assert(admitted.uploads.size()==9 && limited.liveSlotCount()==9 && limited.plan().pendingCount()==1);
    assert(limited.voxelSizes()[0]==fakeStorageLimit);
    limited.evict({0,0,0,0});const auto retry=limited.record(cmd,1);
    assert(retry.uploads.size()==1 && retry.releases.size()==1 && limited.plan().pendingCount()==0);
    assert(limited.voxelSizes()[0]==fakeStorageLimit);
    limited.shutdown();fakeStorageLimit=128u*1024u*1024u;
    std::cout<<"Stage26 bounded dense admission and reuse under SSBO limit PASS (mock API)\n";
    std::cout<<"Stage26 4096 logical slots, high-ID dense update, culler dispatch and scratch contract PASS (mock API)\n";
    }
    material.shutdown();
    assert(argc==2);
    VulkanBlockAtlas atlas;
    atlas.initialize(physical,device,argv[1],16);
    assert(atlas.width()==1024 && atlas.height()==512 && atlas.mipLevels()==5);
    atlas.recordUpload(cmd);
    assert(atlasCopyCalls==1 && atlasBlitRegions==2048*4);
    assert(atlas.createDescriptorSet(reinterpret_cast<VkDescriptorSetLayout>(3)));
    atlas.releaseStagingAfterCompletion();
    atlas.shutdown();
    VulkanBlockAtlas model;
    const std::array<uint8_t,24> rgba{1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24};
    model.initializePixels(physical,device,3,2,rgba.data(),1,true);
    assert(model.width()==3 && model.height()==2 && model.mipLevels()==1);
    model.recordUpload(cmd);
    assert(atlasCopyCalls==2 && atlasBlitRegions==2048*4);
    assert(model.createDescriptorSet(reinterpret_cast<VkDescriptorSetLayout>(3)));
    model.releaseStagingAfterCompletion();model.shutdown();
    std::cout<<"Vulkan Stage 5 mocked transfer/slot/material/atlas and raw entity texture tests passed\n";
}
