// Host-only Vulkan API simulation. It validates descriptor source ABI,
// buffer offsets, batch copying and slot lifecycle. It is NOT GPU validation.
#include "rendering/vulkan/VulkanChunkStreamGpu.h"
#include "rendering/vulkan/VulkanGpuCuller.h"
#include "rendering/vulkan/SurfaceLod.h"
#include "rendering/vulkan/MeshDispatchRuns.h"
#include "rendering/vulkan/VulkanGpuMesher.h"
#include "rendering/vulkan/VulkanResidentLight.h"
#include "rendering/vulkan/ResidentLightScheduler.h"
#include "rendering/vulkan/ResidentSkylightScheduler.h"
#include "rendering/vulkan/VulkanMiscMesher.h"
#include "rendering/vulkan/VulkanMaterialTables.h"
#include "rendering/vulkan/VulkanBlockAtlas.h"

#include "rendering/vulkan/VulkanColumnMaterialRuntime.h"
#include "rendering/vulkan/VulkanEntityPipeline.h"
#include "rendering/vulkan/VulkanItemPipeline.h"
#include <fstream>
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
bool watchColumn=false,watchGraphics=false;
struct ColumnDispatch {uint32_t x,y,z,pushBytes;VkDescriptorSet set;};
std::vector<ColumnDispatch> columnDispatches;
VkDescriptorSet columnSet=VK_NULL_HANDLE;
uint32_t columnPushBytes=0;
struct GraphicsDraw {bool indexed;uint32_t count,instances,first,firstInstance;};
std::vector<GraphicsDraw> graphicsDraws;

struct MeshDispatch {uint32_t x,y;std::array<uint32_t,4> params;};
bool watchMesh=false;
bool watchResidentLight=false;
ResidentLightBatch::Params lightParams{};
std::vector<ResidentLightBatch::Params> lightDispatches;

std::array<uint32_t,4> meshParams{};
std::vector<MeshDispatch> meshDispatches;
struct IndirectMeshDispatch {VkDeviceSize offset;std::array<uint32_t,4> params;};
std::vector<IndirectMeshDispatch> indirectMeshDispatches;
struct MeshBarrier {size_t afterDispatch;VkPipelineStageFlags src,dst;VkAccessFlags sa,da;};
std::vector<MeshBarrier> meshBarriers,lightBarriers;
VkDescriptorSet residentLightSet=VK_NULL_HANDLE;
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
    p->memoryTypeCount=2;p->memoryHeapCount=2;
    p->memoryHeaps[0]={8ull<<30,VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};p->memoryHeaps[1]={16ull<<30,0};
    p->memoryTypes[0].heapIndex=0;p->memoryTypes[1].heapIndex=1;
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
    if(watchResidentLight){assert(n==1);lightBarriers.push_back({lightDispatches.size(),src,dst,barriers[0].srcAccessMask,barriers[0].dstAccessMask});}
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
            if(!watchColumn)assert(info.range==B(info.buffer).size);
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
void vkCmdBindDescriptorSets(VkCommandBuffer,uint32_t,VkPipelineLayout,uint32_t,uint32_t count,const VkDescriptorSet* sets,uint32_t,const uint32_t*){
    if(watchColumn){assert(count==1);columnSet=sets[0];}
    if(watchResidentLight){assert(count==1);residentLightSet=sets[0];}
}
void vkCmdPushConstants(VkCommandBuffer,VkPipelineLayout,uint32_t,uint32_t offset,uint32_t bytes,const void* data){
    if(watchColumn){assert(offset==0);columnPushBytes=bytes;}
    if(watchResidentLight){assert(offset==0&&bytes==sizeof(lightParams));std::memcpy(&lightParams,data,bytes);}
    if(watchMesh){assert(offset==0 && bytes==16);std::memcpy(meshParams.data(),data,16);}
    if(watchCull){assert(offset==0 && bytes==128);std::memcpy(cullParams.data(),static_cast<const std::byte*>(data)+96,16);}
}
void vkCmdDispatch(VkCommandBuffer,uint32_t x,uint32_t y,uint32_t z){
    if(watchColumn)columnDispatches.push_back({x,y,z,columnPushBytes,columnSet});
    if(watchResidentLight){assert(x==(lightParams.readBank==4?4u:128u)&&y==lightParams.jobCount&&z==1);lightDispatches.push_back(lightParams);}
    if(watchMesh){assert(z==1);meshDispatches.push_back({x,y,meshParams});}
    if(watchCull){assert(y==1 && z==1);cullDispatches.push_back({x,cullParams[0],cullParams[1],cullParams[3]});}
}
void vkCmdDispatchIndirect(VkCommandBuffer,VkBuffer args,VkDeviceSize offset){
    assert(offset%4==0 && offset+12<=B(args).size);
    assert(word(args,offset)==0 && word(args,offset+4)==1 && word(args,offset+8)==1);
    if(watchMesh)indirectMeshDispatches.push_back({offset,meshParams});
}
void vkCmdSetViewport(VkCommandBuffer,uint32_t,uint32_t,const VkViewport*){}
void vkCmdSetScissor(VkCommandBuffer,uint32_t,uint32_t,const VkRect2D*){}
void vkCmdBindVertexBuffers(VkCommandBuffer,uint32_t,uint32_t count,const VkBuffer* b,const VkDeviceSize* o){assert(count==1&&aliveBuffers.contains(*b)&&*o==0);}
void vkCmdBindIndexBuffer(VkCommandBuffer,VkBuffer b,VkDeviceSize o,uint32_t type){assert(aliveBuffers.contains(b)&&o==0&&type==VK_INDEX_TYPE_UINT32);}
void vkCmdDraw(VkCommandBuffer,uint32_t count,uint32_t instances,uint32_t first,uint32_t firstInstance){if(watchGraphics)graphicsDraws.push_back({false,count,instances,first,firstInstance});}
void vkCmdDrawIndexed(VkCommandBuffer,uint32_t count,uint32_t instances,uint32_t first,int32_t offset,uint32_t firstInstance){assert(offset==0);if(watchGraphics)graphicsDraws.push_back({true,count,instances,first,firstInstance});}
void vkCmdUpdateBuffer(VkCommandBuffer,VkBuffer dst,VkDeviceSize offset,VkDeviceSize size,const void* data){
    assert(offset+size<=B(dst).size);std::memcpy(B(dst).memory->bytes.data()+offset,data,size);
}

}
int main(int argc,char** argv){
    auto device=reinterpret_cast<VkDevice>(1);
    auto physical=reinterpret_cast<VkPhysicalDevice>(2);
    auto cmd=reinterpret_cast<VkCommandBuffer>(3);
    // Stage47 real recorder, mocked Vulkan API. No shader execution here.
    {
        const auto baseline=aliveBuffers.size();
        VulkanDeviceBuffer blocks,seeds;
        blocks.create(physical,device,65536,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        seeds.create(physical,device,32768,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VulkanResidentLight light;
        const std::array<uint32_t,1> spv{0x07230203u};
        bool rejected=false;
        try{light.initialize(physical,device,65535,spv);}catch(const std::invalid_argument&){rejected=true;}
        assert(rejected&&!light.capacity());
        light.initialize(physical,device,2,spv);
        std::array<GpuChunkSlot,7> slots{};
        slots[5].source={0,0,0,1};slots[5].origin={0,0,0,int32_t(kVoxelAddressFlag)};slots[5].neighborA[1]=2;slots[5].neighborB[3]=0;
        slots[2].source={0x12341234,0,0,1};slots[2].origin={32,0,0,int32_t(kVoxelAddressFlag|kVoxelUniformBlock|kVoxelUniformLight)};slots[2].neighborA[0]=5;slots[2].neighborB[3]=0x10101010;
        const std::array<uint32_t,2> selected{2,5},duplicate{2,2};
        std::array<uint32_t,2048> bits{};bits[3]=0xdeadbeef;
        VulkanResidentLight::Sources inputs{blocks.buffer(),seeds.buffer(),blocks.size(),seeds.size(),11};
        watchResidentLight=true;
        assert(!light.record(cmd,inputs,slots,duplicate,bits));
        auto invalid=inputs;invalid.seedBytes=4;
        assert(!light.record(cmd,invalid,slots,selected,bits));
        assert(lightDispatches.empty()&&lightBarriers.empty()&&!light.busy());
        assert(light.record(cmd,inputs,slots,selected,bits)&&light.busy());
        assert(lightDispatches.size()==14&&lightBarriers.size()==15);
        for(unsigned i=0;i<14;++i){const auto p=lightDispatches[i];assert(p.jobCount==2&&p.readBank==(i?((i-1)&1):2)&&p.writeBank==(i&1));}
        assert(lightBarriers[0].afterDispatch==0&&(lightBarriers[0].sa&VK_ACCESS_TRANSFER_WRITE_BIT));
        for(unsigned i=1;i<14;++i){auto b=lightBarriers[i];assert(b.afterDispatch==i&&b.src==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT&&b.dst==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT&&(b.da&VK_ACCESS_SHADER_READ_BIT)&&(b.da&VK_ACCESS_SHADER_WRITE_BIT));}
        assert(lightBarriers.back().afterDispatch==14&&(lightBarriers.back().da&VK_ACCESS_TRANSFER_READ_BIT));
        const auto& ds=descriptorState.at(residentLightSet)->bindings;
        assert(ds.size()==6&&ds.at(0).buffer==blocks.buffer()&&ds.at(9).buffer==seeds.buffer());
        assert(word(ds.at(1).buffer,0)==0x12341234&&word(ds.at(1).buffer,64)==0);
        assert(word(ds.at(2).buffer,0)==0&&word(ds.at(2).buffer,4)==1&&word(ds.at(2).buffer,32)==1&&word(ds.at(2).buffer,40)==0);
        assert(word(ds.at(3).buffer,12)==0xdeadbeef);
        auto r=light.result(11);assert(r&&r->buffer==ds.at(4).buffer&&r->offset==65536&&r->bytes==65536&&r->epoch==11);
        assert(!light.result(12));
        assert(!light.record(cmd,inputs,slots,selected,bits)&&lightDispatches.size()==14);
        // Test harness simulates fence retirement before descriptor rewrite.
        light.retire();assert(!light.busy()&&!light.result(11));
        inputs.epoch=12;assert(light.record(cmd,inputs,slots,std::span(selected).first(1),bits));
        assert(lightDispatches.size()==28&&light.result(12)->offset==32768&&light.result(12)->bytes==32768);
        light.retire();
        std::array<uint8_t,65536> emission{};
        assert(!light.recordGenerated(cmd,inputs,slots,selected,bits,std::span(emission).first(65535)));
        for(unsigned level=0;level<=255;++level){
            emission[65535]=uint8_t(level);emission[0x1234]=uint8_t(level);
            const unsigned maximum=std::min(level,14u),passes=maximum?maximum-1:0;
            lightDispatches.clear();lightBarriers.clear();
            assert(light.recordGenerated(cmd,inputs,slots,selected,bits,emission));
            assert(lightDispatches.size()==passes+1&&lightBarriers.size()==passes+2);
            for(unsigned i=1;i<=passes;++i){const auto& b=lightBarriers[i];
                assert(b.afterDispatch==i&&b.src==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT&&b.dst==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT&&
                       (b.da&VK_ACCESS_SHADER_READ_BIT)&&(b.da&VK_ACCESS_SHADER_WRITE_BIT));}
            assert(lightBarriers.back().afterDispatch==passes+1&&(lightBarriers.back().da&VK_ACCESS_TRANSFER_READ_BIT));
            unsigned bank=1^(passes&1);
            assert(lightDispatches[0].readBank==3&&lightDispatches[0].writeBank==bank);
            for(unsigned p=0;p<passes;++p){const auto q=lightDispatches[p+1];assert(q.readBank==bank&&q.writeBank==(bank^1));bank^=1;}
            assert(bank==1&&light.result(12)->offset==65536);
            const auto buf=descriptorState.at(residentLightSet)->bindings.at(3).buffer;
            assert(word(buf,8192+8191*4)==(maximum<<28));
            assert(word(buf,8192+(0x1234/8)*4)==(maximum<<16));
            assert(word(buf,12)==0xdeadbeef);
            assert(!light.recordGenerated(cmd,inputs,slots,selected,bits,emission));
            light.retire();
        }
        for(unsigned id=0;id<65536;++id)emission[id]=uint8_t(id*19+id/256);
        std::array<uint32_t,8192> packed{};
        assert(ResidentLightBatch::packEmission(emission,packed)==13);
        for(unsigned id=0;id<65536;++id)assert(((packed[id/8]>>(4*(id%8)))&15u)==std::min(unsigned(emission[id]),14u));
        assert(ResidentLightBatch::packEmission(std::span(emission).first(65535),packed)==ResidentLightBatch::missing);
        // Vertical chain with the upper job second in selection order.
        slots[5].origin[0]=32;slots[5].origin[1]=32;
        slots[5].neighborA[1]=-1;slots[5].neighborA[2]=2;
        slots[2].neighborA[0]=-1;slots[2].neighborA[3]=5;
        std::array<uint32_t,64> top{};top[0]=1;
        lightDispatches.clear();lightBarriers.clear();
        assert(!light.recordSky(cmd,inputs,slots,selected,bits,top)); // interior mask
        top[0]=0;top[32]=0x12345678;
        assert(!light.recordSky(cmd,inputs,slots,selected,bits,std::span(top).first(63)));
        assert(lightDispatches.empty()&&lightBarriers.empty());
        assert(light.recordSky(cmd,inputs,slots,selected,bits,top));
        assert(lightDispatches.size()==15&&lightBarriers.size()==16);
        assert(lightDispatches.front().readBank==4&&lightDispatches.front().writeBank==1);
        for(unsigned i=0;i<14;++i){const auto p=lightDispatches[i+1];assert(p.readBank==5+(1^(i&1))&&p.writeBank==(i&1));}
        assert(word(descriptorState.at(residentLightSet)->bindings.at(3).buffer,8192+32*4)==0x12345678);
        assert(light.result(12)->offset==65536&&light.result(12)->bytes==65536);
        assert(!light.recordSky(cmd,inputs,slots,selected,bits,top));
        light.retire();top.fill(0);lightDispatches.clear();lightBarriers.clear();
        assert(light.recordSky(cmd,inputs,slots,selected,bits,top));
        assert(lightDispatches.size()==1&&lightBarriers.size()==2&&light.result(12)->offset==65536);
        light.retire();
        watchResidentLight=false;
        assert(aliveBuffers.size()==baseline+6);
    }
    assert(aliveBuffers.empty());
    std::cout<<"Stage47/48 resident light recorder: legacy and 256 generated emission levels, descriptor ABI, barriers, retirement and epoch guards passed\n";
    // Stage49: complete halo, ordered core prefix, reject incomplete snapshots.
    {
        VulkanDeviceBuffer blocks,seeds;
        blocks.create(physical,device,65536,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        seeds.create(physical,device,32768,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VulkanResidentLight light;
        const std::array<uint32_t,1> spv{0x07230203u};light.initialize(physical,device,36,spv);
        std::vector<GpuChunkSlot> slots(75);
        auto id=[](int x,int y,int z){return uint32_t(2+2*(x*9+y*3+z));};
        for(int x=0;x<4;++x)for(int y=0;y<3;++y)for(int z=0;z<3;++z){
            auto& s=slots[id(x,y,z)];s.source={0,0,0,1};
            s.origin={32*x-96,32*y-64,32*z-32,int32_t(kVoxelAddressFlag|kVoxelUniformBlock|kVoxelUniformLight)};
            s.neighborA={x?int32_t(id(x-1,y,z)):-1,x<3?int32_t(id(x+1,y,z)):-1,
                         y?int32_t(id(x,y-1,z)):-1,y<2?int32_t(id(x,y+1,z)):-1};
            s.neighborB={z?int32_t(id(x,y,z-1)):-1,z<2?int32_t(id(x,y,z+1)):-1,0,0};
        }
        const std::array<uint32_t,2> core{id(2,1,1),id(1,1,1)};
        std::vector<uint32_t> selected;
        auto select=[&](std::span<const uint32_t> c,uint32_t limit){return ResidentLightBatch::selectCore<GpuChunkSlot>(slots,c,limit,selected);};
        assert(select(std::span(core).first(1),27)&&selected.size()==27&&selected[0]==core[0]);
        assert(select(core,36)&&selected.size()==36&&selected[0]==core[0]&&selected[1]==core[1]);
        assert(!select(core,35)&&selected.empty());
        const std::array<uint32_t,2> duplicate{core[0],core[0]};assert(!select(duplicate,36));
        const std::array<uint32_t,1> edge{id(0,1,1)};assert(!select(edge,36));
        slots[id(3,2,2)].source[3]=0;assert(!select(core,36));slots[id(3,2,2)].source[3]=1;
        slots[1]=slots[core[0]];assert(!select(core,36));slots[1]={};
        std::array<uint32_t,2048> bits{};std::array<uint8_t,65536> emission{};emission[7]=14;
        VulkanResidentLight::Sources inputs{blocks.buffer(),seeds.buffer(),blocks.size(),seeds.size(),77};
        watchResidentLight=true;lightDispatches.clear();lightBarriers.clear();
        slots[core[0]].neighborA[0]=-1;
        assert(!light.recordGeneratedCore(cmd,inputs,slots,core,bits,emission));
        assert(lightDispatches.empty()&&!light.busy());slots[core[0]].neighborA[0]=int32_t(core[1]);
        assert(light.recordGeneratedCore(cmd,inputs,slots,core,bits,emission));
        assert(lightDispatches.size()==14&&lightBarriers.size()==15&&lightDispatches[0].jobCount==36);
        const auto all=light.result(77),r=light.resultCore(77);
        assert(all&&r&&all->offset==36*32768u&&r->offset==all->offset&&r->bytes==2*32768u&&all->bytes==36*32768u);
        assert(!light.resultCore(78)&&!light.recordGeneratedCore(cmd,inputs,slots,core,bits,emission));
        const auto& ds=descriptorState.at(residentLightSet)->bindings;
        assert(word(ds.at(1).buffer,16)==uint32_t(slots[core[0]].origin[0]));
        assert(word(ds.at(1).buffer,64+16)==uint32_t(slots[core[1]].origin[0]));
        light.retire();assert(!light.resultCore(77));
        assert(light.recordGenerated(cmd,inputs,slots,core,bits,emission)&&!light.resultCore(77));
        light.retire();
        assert(select(core,36));std::vector<uint32_t> skyMasks(selected.size()*32);
        for(size_t i=0;i<selected.size();++i)if(slots[selected[i]].neighborA[3]==-1)skyMasks[i*32]=1;
        lightDispatches.clear();lightBarriers.clear();
        assert(!light.recordSkyCore(cmd,inputs,slots,std::span(selected).first(35),2,bits,std::span(skyMasks).first(35*32)));
        assert(!light.recordSkyCore(cmd,inputs,slots,selected,0,bits,skyMasks));
        assert(lightDispatches.empty()&&!light.busy());
        assert(light.recordSkyCore(cmd,inputs,slots,selected,2,bits,skyMasks));
        assert(light.resultCore(77)->bytes==2*32768u&&light.resultCore(77)->offset==36*32768u);
        assert(lightDispatches.size()==15);light.retire();watchResidentLight=false;
    }
    assert(aliveBuffers.empty());
    std::cout<<"Stage49 complete halo admission, core prefix ranges, busy/epoch guards passed\n";
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
    auto lightFixed=u;lightFixed.lightOnly=true;lightFixed.blockWords.clear();lightFixed.metaWords.clear();
    ++lightFixed.meshVersion;lightFixed.lightWords.assign(kChunkVoxels/4,0x22446688);
    auto fixedStats=compact.uploadStats();compact.submit({lightFixed});compact.record(cmd,1);
    u.lightWords=lightFixed.lightWords;checkPacket(0,u);checkPacket(1,adjacent);
    assert(compact.uploadStats().copiedBytes-fixedStats.copiedBytes==32768);
    assert(compact.uploadStats().filledBytes==fixedStats.filledBytes);
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

    // Stage55: the CPU payload is always retained, but valid immutable GPU
    // snapshots bypass staging for dense block/meta channels. Use an arena
    // growth in the same record, and keep the source alive through frame 1.
    growing.initialize(physical,device,tiny);watchedLive=growing.liveSlotIndices();
    growing.submit({denseA});growing.record(cmd,0);
    auto gpuPacket=packet(1,31,1,0);
    for(size_t i=0;i<gpuPacket.blockWords.size();++i)gpuPacket.blockWords[i]=uint32_t(i*17+3);
    for(size_t i=0;i<gpuPacket.metaWords.size();++i)gpuPacket.metaWords[i]=uint32_t(i*313+9);
    auto source=std::make_shared<OverlaySnapshot>();source->device=device;source->ticket=123;source->ready=true;
    source->buffer.create(physical,device,98304,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    source->buffer.write(0,gpuPacket.blockWords.data(),65536);
    source->buffer.write(65536,gpuPacket.metaWords.data(),32768);
    gpuPacket.overlayReference={source,123};
    const auto sourceHandle=source->buffer.buffer();
    const auto beforeStats=growing.uploadStats();
    growing.submit({gpuPacket});growing.record(cmd,1);
    const auto gpuOffsets=verifyDense(gpuPacket);assert(gpuOffsets[0]!=0&&gpuOffsets[1]!=0);
    assert(growing.uploadStats().overlayReusedBytes-beforeStats.overlayReusedBytes==98304);
    assert(growing.uploadStats().copiedBytes-beforeStats.copiedBytes==32768);
    assert(source.use_count()==2);
    source.reset();assert(aliveBuffers.contains(sourceHandle));
    growing.record(cmd,0);assert(aliveBuffers.contains(sourceHandle));
    growing.record(cmd,1);assert(!aliveBuffers.contains(sourceHandle));
    // Stage56: camera-only control carries no voxel data and preserves
    // physical contents, including a source which used a GPU snapshot.
    auto control=gpuPacket;control.lodOnly=true;control.overlayReference={};
    control.blockWords.clear();control.metaWords.clear();control.lightWords.clear();
    ++control.lodRevision;control.meshKinds|=kSurfaceLodFlag;
    const auto controlStats=growing.uploadStats();
    growing.submit({control});auto controlBatch=growing.record(cmd,0);
    assert(controlBatch.uploads.size()==1 && !controlBatch.uploads[0].uploadVoxels);
    assert(controlBatch.remeshSlots==std::vector<uint32_t>{growing.plan().lookup(gpuPacket.position)->slot});
    assert(verifyDense(gpuPacket)==gpuOffsets);
    assert(growing.uploadStats().copiedBytes==controlStats.copiedBytes && growing.uploadStats().overlayReusedBytes==controlStats.overlayReusedBytes);
    // Stage57: light-only update after snapshot expiration preserves dense
    // block/meta addresses and bytes; only the light page is written.
    auto lightUpdate=control;lightUpdate.lodOnly=false;lightUpdate.lightOnly=true;
    ++lightUpdate.meshVersion;++lightUpdate.lodRevision;
    lightUpdate.lightWords=gpuPacket.lightWords;lightUpdate.lightWords[999]=0x87654321;
    auto lightExpected=gpuPacket;lightExpected.lightWords=lightUpdate.lightWords;
    const auto lightStats=growing.uploadStats();
    growing.submit({lightUpdate});auto lightBatch=growing.record(cmd,1);
    assert(lightBatch.uploads.size()==1 && lightBatch.uploads[0].packet.lightOnly);
    assert(verifyDense(lightExpected)==gpuOffsets);
    assert(growing.uploadStats().copiedBytes-lightStats.copiedBytes==32768);
    assert(growing.uploadStats().overlayReusedBytes==lightStats.overlayReusedBytes);
    // Dense -> inline -> GPU-owned dense fill -> CPU-owned inline handback.
    ++lightUpdate.meshVersion;++lightUpdate.lodRevision;lightUpdate.lightWords={0x34343434};
    lightExpected.lightWords=lightUpdate.lightWords;
    growing.submit({lightUpdate});growing.record(cmd,0);auto uniformOffsets=verifyDense(lightExpected);
    assert(uniformOffsets[0]==gpuOffsets[0] && uniformOffsets[1]==gpuOffsets[1]);
    lightUpdate.gpuBlockLightOwned=true;++lightUpdate.lightOwnershipRevision;++lightUpdate.lodRevision;
    growing.submit({lightUpdate});growing.record(cmd,1);
    auto residentLight=growing.plan().lookup(gpuPacket.position).value();
    auto entry=reinterpret_cast<const GpuChunkSlot*>(B(growing.opaqueSlots()).memory->bytes.data())[residentLight.slot];
    assert(!(entry.origin[3]&kVoxelUniformLight));
    for(size_t i=0;i<8192;++i)assert(word(growing.lights(),(uint32_t(entry.neighborB[3])+i)*4)==0x34343434);
    lightUpdate.gpuBlockLightOwned=false;++lightUpdate.lightOwnershipRevision;++lightUpdate.lodRevision;
    growing.submit({lightUpdate});growing.record(cmd,0);assert(verifyDense(lightExpected)==uniformOffsets);
    std::cout<<"Stage57 independent light upload: 98304 preserved block/meta bytes, dense/inline/owned/handback PASS (mock API)\n";
    // Expired reference and a newer edited payload must use staging.
    auto changed=gpuPacket;changed.blocksVersion++;changed.meshVersion=lightUpdate.meshVersion+1;changed.lightOwnershipRevision=lightUpdate.lightOwnershipRevision;changed.blockWords[777]=0x12345678;
    growing.submit({changed});growing.record(cmd,0);verifyDense(changed);
    assert(growing.uploadStats().overlayReusedBytes==98304);
    // Valid live source but stale ticket, or incompatible device: no GPU copy.
    auto stale=std::make_shared<OverlaySnapshot>();stale->device=device;stale->ticket=9;stale->ready=true;
    stale->buffer.create(physical,device,98304,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    changed.blocksVersion++;changed.meshVersion++;changed.metaWords[99]=0xaabbccdd;
    changed.overlayReference={stale,8};growing.submit({changed});growing.record(cmd,1);verifyDense(changed);
    assert(growing.uploadStats().overlayReusedBytes==98304);
    changed.blocksVersion++;changed.meshVersion++;changed.overlayReference={stale,9};stale->device=nullptr;
    growing.submit({changed});growing.record(cmd,0);verifyDense(changed);assert(growing.uploadStats().overlayReusedBytes==98304);
    // Uniform metadata remains inline even if an otherwise valid source exists.
    stale->device=device;changed.blocksVersion++;changed.meshVersion++;changed.metaWords={0};
    stale->buffer.write(0,changed.blockWords.data(),65536);
    growing.submit({changed});growing.record(cmd,1);verifyDense(changed);
    assert(growing.uploadStats().overlayReusedBytes==98304+65536);
    growing.shutdown();stale.reset();
    std::cout<<"Stage55 resident overlay copies: dense offsets, growth, fence lease, stale/device/expired fallback, inline metadata PASS (mock API)\n";

    // Stage58: source-only admission, no fallback block/meta CPU arrays.
    // Queued lease survives backpressure, coalesced controls and frame fences.
    growing.initialize(physical,device,tiny);watchedLive=growing.liveSlotIndices();
    growing.submit({denseA});growing.record(cmd,0);
    auto only=packet(1,51,1,0x12341234);only.metaWords[15]=0x01020304;
    auto onlySource=std::make_shared<OverlaySnapshot>();onlySource->device=device;
    onlySource->ticket=501;onlySource->ready=true;
    onlySource->buffer.create(physical,device,98304,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    onlySource->buffer.write(0,only.blockWords.data(),65536);
    onlySource->buffer.write(65536,only.metaWords.data(),32768);
    auto sourcePacket=only;sourcePacket.blockWords.clear();sourcePacket.metaWords.clear();
    sourcePacket.overlayReference={onlySource,500};
    assert(!growing.submitGpuSnapshot(LiveChunkPacket(sourcePacket)) && growing.plan().pendingCount()==0);
    sourcePacket.overlayReference.ticket=501;
    const auto onlyHandle=onlySource->buffer.buffer();
    assert(growing.submitGpuSnapshot(LiveChunkPacket(sourcePacket)));
    assert(onlySource.use_count()==2);
    onlySource.reset();assert(aliveBuffers.contains(onlyHandle));
    growing.record(cmd,1,false);assert(growing.plan().pendingCount()==1 && aliveBuffers.contains(onlyHandle));
    auto delta=sourcePacket;delta.overlayReference={};delta.lightOnly=true;
    ++delta.meshVersion;++delta.lodRevision;delta.lightWords[99]=0x67676767;
    growing.submit({delta});only.lightWords=delta.lightWords;
    auto lod=delta;lod.lightOnly=false;lod.lodOnly=true;lod.lightWords.clear();
    ++lod.lodRevision;lod.meshKinds|=kSurfaceLodFlag;growing.submit({lod});
    const auto sourceStats=growing.uploadStats();const auto knownBoundary=growing.plan().boundaryPayloadBytes();
    auto copied=growing.record(cmd,0);assert(copied.uploads.size()==1);
    assert(copied.uploads[0].packet.blockWords.empty() && copied.uploads[0].packet.metaWords.empty());
    assert(!copied.uploads[0].packet.gpuSourceLease);
    assert(growing.plan().lookup(only.position)->lodRevision==lod.lodRevision);
    assert(growing.uploadStats().overlayReusedBytes-sourceStats.overlayReusedBytes==98304);
    assert(growing.uploadStats().copiedBytes-sourceStats.copiedBytes==32768);
    const auto sourceOffsets=verifyDense(only);
    assert(growing.plan().boundaryPayloadBytes()==knownBoundary);
    growing.record(cmd,1);assert(aliveBuffers.contains(onlyHandle));
    growing.record(cmd,0);assert(!aliveBuffers.contains(onlyHandle));
    // Original source buffer is gone, and the planner still has no CPU
    // boundary payload for the source-only resident. Read from the arena.
    const auto sourceOnlyResident=growing.plan().lookup(only.position).value();
    failAllocationSize=131072;
    assert(!growing.recordResidentReadback(cmd,0,{sourceOnlyResident.slot,sourceOnlyResident.generation}));
    failAllocationSize=0;
    watchResidentLight=true;lightBarriers.clear();
    assert(growing.recordResidentReadback(cmd,0,{sourceOnlyResident.slot,sourceOnlyResident.generation}));
    assert(lightBarriers.size()==2);
    assert((lightBarriers[0].sa&VK_ACCESS_TRANSFER_WRITE_BIT)&&(lightBarriers[0].da&VK_ACCESS_TRANSFER_READ_BIT));
    assert((lightBarriers[1].sa&VK_ACCESS_TRANSFER_WRITE_BIT)&&(lightBarriers[1].da&VK_ACCESS_HOST_READ_BIT));
    assert(lightBarriers[1].dst==VK_PIPELINE_STAGE_HOST_BIT);
    watchResidentLight=false;
    growing.record(cmd,1);assert(!growing.takeResidentReadback(0));
    growing.record(cmd,0);auto sourceOnlyRead=growing.takeResidentReadback(0);
    assert(sourceOnlyRead&&growing.residentReadbackCurrent(*sourceOnlyRead));
    assert(sourceOnlyRead->words[0]==only.blockWords&&sourceOnlyRead->words[1]==only.metaWords&&sourceOnlyRead->words[2]==only.lightWords);
    // Unknown GPU boundaries must never suppress neighboring remeshes.
    auto diagonal=packet(2,52,1,0);diagonal.position.z=1;
    growing.submit({diagonal});growing.record(cmd,1);
    const auto diagonalSlot=growing.plan().lookup(diagonal.position)->slot;
    ++delta.meshVersion;delta.lodRevision=lod.lodRevision+1;delta.meshKinds=lod.meshKinds;
    delta.lightWords[100]=0xabcdef01;only.lightWords=delta.lightWords;
    growing.submit({delta});auto sourceUpdated=growing.record(cmd,0);
    assert(std::find(sourceUpdated.remeshSlots.begin(),sourceUpdated.remeshSlots.end(),diagonalSlot)!=sourceUpdated.remeshSlots.end());
    assert(verifyDense(only)==sourceOffsets);
    // Full CPU packet restores exact boundary tracking.
    only.meshVersion=delta.meshVersion+1;only.lodRevision=delta.lodRevision+1;only.meshKinds=delta.meshKinds;
    const auto unknownBytes=growing.plan().boundaryPayloadBytes();
    growing.submit({only});growing.record(cmd,1);assert(verifyDense(only)==sourceOffsets);
    assert(growing.plan().boundaryPayloadBytes()>unknownBytes);
    // Leased block-only packet retains uniform metadata and releases source
    // immediately when evicted before admission.
    auto cancelSource=std::make_shared<OverlaySnapshot>();cancelSource->device=device;
    cancelSource->ticket=502;cancelSource->ready=true;
    cancelSource->buffer.create(physical,device,98304,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    cancelSource->buffer.write(0,only.blockWords.data(),65536);
    auto blockOnly=only;blockOnly.position.x=3;blockOnly.chunkInstance=53;blockOnly.blockWords.clear();blockOnly.metaWords={0};
    blockOnly.overlayReference={cancelSource,502};
    blockOnly.gpuSourceLease=growing.snapshotLeaseSink()(blockOnly.overlayReference);
    const auto cancelHandle=cancelSource->buffer.buffer();
    growing.submit({std::move(blockOnly)});cancelSource.reset();assert(aliveBuffers.contains(cancelHandle));
    growing.evict({0,3,0,0});assert(!aliveBuffers.contains(cancelHandle));
    // An actual block-only admission must not allocate a dense metadata page.
    auto inlineSource=std::make_shared<OverlaySnapshot>();inlineSource->device=device;
    inlineSource->ticket=503;inlineSource->ready=true;
    inlineSource->buffer.create(physical,device,98304,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    inlineSource->buffer.write(0,only.blockWords.data(),65536);
    auto inlineExpected=only;inlineExpected.position.x=3;inlineExpected.chunkInstance=54;inlineExpected.metaWords={0};
    auto inlinePacket=inlineExpected;inlinePacket.blockWords.clear();inlinePacket.overlayReference={inlineSource,503};
    const auto inlineStats=growing.uploadStats();
    assert(growing.submitGpuSnapshot(std::move(inlinePacket)));
    growing.record(cmd,0);verifyDense(inlineExpected);
    assert(growing.uploadStats().overlayReusedBytes-inlineStats.overlayReusedBytes==65536);
    auto expiredCopy=inlineExpected;expiredCopy.position.x=4;expiredCopy.blockWords.clear();
    expiredCopy.overlayReference={inlineSource,999};expiredCopy.gpuSourceLease=inlineSource;
    bool rejectedLease=false;
    try{growing.submit({packet(5,55,1,0),expiredCopy});}catch(const LodControlSourceMissing&){rejectedLease=true;}
    assert(rejectedLease && growing.plan().pendingCount()==0);
    expiredCopy.gpuSourceLease.reset();inlineSource.reset();
    // Stage59: source-only resident round trip without CPU source lease.
    auto sourceResident=growing.plan().lookup(only.position).value();
    const ChunkHandle readHandle{sourceResident.slot,sourceResident.generation};
    assert(!growing.recordResidentReadback(cmd,1,readHandle));
    assert(!growing.recordResidentReadback(cmd,0,{readHandle.slot,readHandle.generation+1}));
    auto readCopies=copies;
    assert(growing.recordResidentReadback(cmd,0,readHandle)&&copies==readCopies+3);
    assert(!growing.recordResidentReadback(cmd,0,readHandle));
    assert(!growing.takeResidentReadback(0));
    growing.record(cmd,1);assert(!growing.takeResidentReadback(0));
    // Concurrent frame slot, inline metadata: only block and light copied.
    auto inlineResident=growing.plan().lookup(inlineExpected.position).value();
    readCopies=copies;
    assert(growing.recordResidentReadback(cmd,1,{inlineResident.slot,inlineResident.generation})&&copies==readCopies+2);
    // LOD-only changes do not stale the bytes.
    auto readLod=only;readLod.lodOnly=true;readLod.blockWords.clear();readLod.metaWords.clear();readLod.lightWords.clear();
    ++readLod.lodRevision;readLod.meshKinds^=kSurfaceLodFlag;
    growing.submit({readLod});growing.record(cmd,0);
    auto readResult=growing.takeResidentReadback(0);assert(readResult&&growing.residentReadbackCurrent(*readResult));
    assert(readResult->words[0]==only.blockWords&&readResult->words[1]==only.metaWords&&readResult->words[2]==only.lightWords);
    assert(!growing.takeResidentReadback(0));
    // Readback is a historical copy even if its slot is evicted before polling.
    growing.evict(inlineExpected.position);growing.record(cmd,1);
    auto oldRead=growing.takeResidentReadback(1);assert(oldRead&&!growing.residentReadbackCurrent(*oldRead));
    assert(oldRead->words[0]==inlineExpected.blockWords&&oldRead->words[1]==inlineExpected.metaWords&&oldRead->words[2]==inlineExpected.lightWords);
    auto uniformRead=packet(4,99,1,0);uniformRead.blockWords={0x12345678};uniformRead.metaWords={0x01020304};uniformRead.lightWords={0xabcdef01};
    growing.submit({uniformRead});growing.record(cmd,0);
    auto uniformResident=growing.plan().lookup(uniformRead.position).value();readCopies=copies;
    assert(growing.recordResidentReadback(cmd,0,{uniformResident.slot,uniformResident.generation})&&copies==readCopies);
    growing.record(cmd,0);auto uniformResult=growing.takeResidentReadback(0);
    assert(uniformResult&&growing.residentReadbackCurrent(*uniformResult));
    assert(uniformResult->words[0]==uniformRead.blockWords&&uniformResult->words[1]==uniformRead.metaWords&&uniformResult->words[2]==uniformRead.lightWords);
    assert(growing.residentReadbackQuiescent(*readResult));
    auto modified=only;++modified.meshVersion;modified.lightWords[0]^=0xff;
    growing.submit({modified});
    assert(growing.residentReadbackCurrent(*readResult)&&!growing.residentReadbackQuiescent(*readResult));
    assert(growing.residentReadbackQuiescent(*uniformResult)); // unrelated pending update
    growing.record(cmd,0);assert(!growing.residentReadbackCurrent(*readResult));
    growing.evict(uniformRead.position);
    assert(growing.residentReadbackCurrent(*uniformResult)&&!growing.residentReadbackQuiescent(*uniformResult));
    growing.shutdown();assert(!growing.residentReadbackCurrent(*uniformResult));
    // Legacy fixed-address pages have no inline representation, read all three.
    growing.initialize(physical,device,{2,2,4,false});growing.submit({uniformRead});growing.record(cmd,0);
    auto fixedRead=growing.plan().lookup(uniformRead.position).value();
    assert(growing.recordResidentReadback(cmd,0,{fixedRead.slot,fixedRead.generation}));
    growing.record(cmd,0);auto fixedResult=growing.takeResidentReadback(0);assert(fixedResult);
    for(size_t k=0;k<3;++k){const auto w=k==0?uniformRead.blockWords[0]:k==1?uniformRead.metaWords[0]:uniformRead.lightWords[0];
        assert(fixedResult->words[k].size()==(k==0?16384u:8192u));
        assert(std::all_of(fixedResult->words[k].begin(),fixedResult->words[k].end(),[&](uint32_t v){return v==w;}));}
    assert(!growing.residentReadbackCurrent(*uniformResult));
    std::cout<<"Stage59 resident readback: dense, inline, fence, OOM, stale, LOD and fixed pages PASS (mock API)\n";
    growing.shutdown();
    std::cout<<"Stage58 GPU-source packet: 98304 byte GPU copy, queued lease, controls, unknown boundary, fallback and eviction PASS (mock API)\n";

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
    // Real stream and mesh recorder: normal uploads are paused, but an
    // interactive packet enters an idle compute lane before queued terrain.
    {
        VulkanChunkStreamGpu edits;edits.initialize(physical,device,{16,8,32,true,true,{1,1,1}});
        VulkanChunkMeshing mesh;auto cfg=meshConfig;cfg.batchLimit=1;cfg.workBudget=8;cfg.fastOpaque=false;
        mesh.initialize(physical,device,edits,fullMaterials,cfg,{fakeSpirv,fakeSpirv,fakeSpirv,fakeSpirv,fakeSpirv,fakeSpirv});
        std::vector<LiveChunkPacket> normal;
        for(int i=0;i<5;++i){auto p=packet(i*2,1,1,0x00020002);p.meshKinds=1;normal.push_back(std::move(p));}
        edits.submit(std::move(normal));auto batch=edits.record(cmd,0);mesh.selectVoxelFrame(0);
        assert(mesh.record(cmd,0,batch)>0 && mesh.pendingCount()==4 && !mesh.acceptsUploads());
        auto update=packet(8,1,2,0x00030003);update.meshKinds=1;update.interactive=true;
        edits.submit({update});auto urgent=edits.record(cmd,1,false);mesh.selectVoxelFrame(1);
        assert(urgent.uploads.size()==1 && urgent.interactiveRemeshSlots.size()==1);
        assert(mesh.record(cmd,1,urgent)>0 && mesh.stats().interactiveStarted==1 && mesh.inFlightCount()==2);
        assert(mesh.pendingCount()==3);
        mesh.shutdown();edits.shutdown();
        std::cout<<"Stage135 interactive stream bypass and actual mesh recorder priority PASS (mock API)\n";
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
    // Stage50: actual stream-owned same-command snapshot, no hand-built slot table.
    {
        VulkanChunkStreamGpu live;
        live.initialize(physical,device,{40,40,80,true,true,{1,1,1}});
        VulkanResidentLight light;
        const std::array<uint32_t,1> spv{0x07230203u};light.initialize(physical,device,27,spv);
        const auto otherCmd=reinterpret_cast<VkCommandBuffer>(4);
        std::array<uint32_t,2048> bits{};std::array<uint8_t,65536> emission{};emission[7]=14;
        auto make=[](ChunkKey k,uint64_t instance,uint64_t version,bool dense){
            LiveChunkPacket p;p.position=k;p.chunkInstance=instance;p.blocksVersion=p.meshVersion=version;
            p.blockWords={0x00070007};p.metaWords={0};p.lightWords={0xf1f1f1f1};
            if(dense){p.blockWords.resize(kChunkBlockWords,0);p.lightWords.resize(kChunkVoxels/4,0xf0f0f0f0);}
            return p;
        };
        const ChunkKey center{0,-4,2,-7},neighbor{0,-3,2,-7},corner{0,-5,1,-8};
        std::vector<LiveChunkPacket> packets;
        for(int x=-1;x<=1;++x)for(int y=-1;y<=1;++y)for(int z=-1;z<=1;++z){
            auto k=center;k.x+=x;k.y+=y;k.z+=z;packets.push_back(make(k,packets.size()+1,1,k==center));
        }
        auto otherDimension=center;otherDimension.dimension=1;
        packets.push_back(make(otherDimension,100,1,false));
        const std::array<ChunkHandle,1> invalid{{{0,1}}};
        assert(!light.recordStreamCore(cmd,0,live,invalid,bits,emission));
        live.submit(std::move(packets));assert(live.record(cmd,0).uploads.size()==28);
        auto r=live.plan().lookup(center).value();
        std::array<ChunkHandle,1> core{{{r.slot,r.generation}}};
        const auto other=live.plan().lookup(otherDimension).value();
        const std::array<ChunkHandle,2> mixed{{core[0],{other.slot,other.generation}}};
        const std::array<ChunkHandle,2> duplicate{{core[0],core[0]}};
        const auto run=[&](VkCommandBuffer c,uint32_t f){return light.recordStreamCore(c,f,live,core,bits,emission);};
        watchResidentLight=true;lightDispatches.clear();lightBarriers.clear();
        assert(!run(otherCmd,0)&&!run(cmd,1));
        assert(!light.recordStreamCore(cmd,0,live,mixed,bits,emission));
        assert(!light.recordStreamCore(cmd,0,live,duplicate,bits,emission));
        assert(lightDispatches.empty()&&run(cmd,0));
        assert(lightDispatches.size()==14&&lightDispatches[0].jobCount==27);
        auto result=light.resultStreamCore(cmd,0,live);assert(result&&result->bytes==32768&&result->offset==27*32768u);
        assert(!light.result(result->epoch)&&!light.resultCore(result->epoch));
        assert(!light.resultStreamCore(otherCmd,0,live)&&!light.resultStreamCore(cmd,1,live));
        const auto& ds=descriptorState.at(residentLightSet)->bindings;
        assert(ds.at(0).buffer==live.blocks()&&ds.at(9).buffer==live.lights());
        const auto* table=reinterpret_cast<const GpuChunkSlot*>(B(live.opaqueSlots()).memory->bytes.data());
        const auto snapshot=ds.at(1).buffer;
        assert(word(snapshot,0)==table[r.slot].source[0]&&word(snapshot,60)==uint32_t(table[r.slot].neighborB[3]));
        assert(word(snapshot,16)==uint32_t(center.x*32));
        // Producer queues an update; current recorded frame is unchanged until take().
        const auto nr=live.plan().lookup(neighbor).value();
        live.submit({make(neighbor,nr.instance,2,true)});
        assert(light.resultStreamCore(cmd,0,live)&&!run(cmd,0));
        watchResidentLight=false;
        const auto oldBlocks=live.blocks(),oldLights=live.lights();const auto oldEpoch=live.voxelEpoch();
        watchResidentLight=true;lightBarriers.clear();
        assert(live.record(otherCmd,1).uploads.size()==1&&live.voxelEpoch()>oldEpoch);
        assert(lightBarriers.size()==3);
        const auto& before=lightBarriers.front();
        assert((before.src&VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)&&
               (before.sa&VK_ACCESS_SHADER_READ_BIT)&&(before.dst&VK_PIPELINE_STAGE_TRANSFER_BIT)&&
               (before.da&VK_ACCESS_TRANSFER_WRITE_BIT)&&(before.da&VK_ACCESS_TRANSFER_READ_BIT));
        watchResidentLight=false;
        assert(live.blocks()!=oldBlocks&&live.lights()!=oldLights);
        assert(aliveBuffers.contains(oldBlocks)&&aliveBuffers.contains(oldLights));
        assert(!light.resultStreamCore(cmd,0,live)&&!light.resultStreamCore(otherCmd,1,live));
        assert(!run(otherCmd,1)); // still busy, caller must wait for its fence
        light.retire(); // mock: fence completed
        watchResidentLight=true;lightDispatches.clear();lightBarriers.clear();
        assert(run(otherCmd,1)&&light.resultStreamCore(otherCmd,1,live));
        assert(descriptorState.at(residentLightSet)->bindings.at(0).buffer==live.blocks());
        watchResidentLight=false;light.retire();
        assert(live.record(otherCmd,1).empty()); // reused handle, new serial; migration fence waited
        assert(!aliveBuffers.contains(oldBlocks)&&!aliveBuffers.contains(oldLights));
        assert(run(otherCmd,1));
        assert(live.record(otherCmd,1).empty()&&!light.resultStreamCore(otherCmd,1,live));
        light.retire();
        // Missing halo is rejected; later refill restores admission.
        live.evict(corner);assert(live.record(cmd,0).releases.size()==1);
        assert(!run(cmd,0));
        live.submit({make(corner,201,1,false)});assert(live.record(cmd,0).uploads.size()==1);
        assert(run(cmd,0));light.retire();
        // Same logical slot reused for a new instance is not a valid core handle.
        live.submit({make(center,202,1,false)});assert(live.record(cmd,0).uploads.size()==1);
        const auto newer=live.plan().lookup(center).value();assert(newer.slot==r.slot&&newer.generation!=r.generation);
        assert(!run(cmd,0));core[0]={newer.slot,newer.generation};assert(run(cmd,0));
        light.retire();live.shutdown();assert(!light.resultStreamCore(cmd,0,live)&&!run(cmd,0));
        // The legacy fixed-address stream is not supported by resident-light ABI.
        live.initialize(physical,device,{2,2,4,false});live.record(cmd,0);
        assert(!run(cmd,0));live.shutdown();
    }
    std::cout<<"Stage50 stream-owned halo snapshot, dimensions, generations, same-command guard, migration and retirement PASS (mock API)\n";
    // Stage51: explicitly owned dense core publication; mock supplies a known
    // scratch result (actual shader parity is exercised by Stage49 EGL).
    {
        VulkanChunkStreamGpu live;
        live.initialize(physical,device,{40,40,80,true,true,{1,1,1}});
        VulkanResidentLight light;
        const std::array<uint32_t,1> spv{0x07230203u};light.initialize(physical,device,36,spv);
        std::array<uint32_t,2048> bits{};std::array<uint8_t,65536> emission{};emission[7]=14;
        const ChunkKey a{0,-4,2,-7},b{0,-3,2,-7},halo{0,-5,1,-8};
        auto make=[](ChunkKey key,uint64_t version,bool owned){
            LiveChunkPacket p;p.position=key;p.chunkInstance=1;p.blocksVersion=p.meshVersion=version;
            p.blockWords={0x00070007};p.metaWords={0};p.lightWords={0xf1f1f1f1};p.gpuBlockLightOwned=owned;return p;
        };
        std::vector<LiveChunkPacket> packets;
        for(int x=-1;x<=2;++x)for(int y=-1;y<=1;++y)for(int z=-1;z<=1;++z){
            auto k=a;k.x+=x;k.y+=y;k.z+=z;auto p=make(k,1,k==a);
            if(k==halo)p.lightWords.resize(kChunkVoxels/4,0xa7a7a7a7);
            packets.push_back(std::move(p));
        }
        live.submit(std::move(packets));auto changed=live.record(cmd,0);assert(changed.uploads.size()==36);
        auto ar=live.plan().lookup(a).value(),br=live.plan().lookup(b).value();
        assert(ar.gpuBlockLightOwned&&!br.gpuBlockLightOwned);
        const std::array<ChunkHandle,2> core{{{ar.slot,ar.generation},{br.slot,br.generation}}};
        auto slot=[&](uint32_t s){GpuChunkSlot out;std::memcpy(&out,B(live.opaqueSlots()).memory->bytes.data()+s*sizeof(out),sizeof(out));return out;};
        assert(!(slot(ar.slot).origin[3]&kVoxelUniformLight));assert(slot(br.slot).origin[3]&kVoxelUniformLight);
        auto aOffset=VkDeviceSize(uint32_t(slot(ar.slot).neighborB[3]))*4;
        assert(word(live.lights(),aOffset)==0xf1f1f1f1&&word(live.lights(),aOffset+32764)==0xf1f1f1f1);
        auto run=[&](std::span<const ChunkHandle> c){return light.recordStreamCore(cmd,0,live,c,bits,emission);};
        auto ncopy=copies;
        assert(run(core));assert(!light.publishStreamCore(cmd,0,live,changed)&&copies==ncopy);
        light.retire();
        // Change ownership through a newer CPU source revision; initial inline
        // light expands into a full dense page at admission, not publication.
        live.submit({make(b,2,true)});auto newer=live.record(cmd,0);assert(newer.uploads.size()==1);
        assert(!(slot(br.slot).origin[3]&kVoxelUniformLight));
        auto bOffset=VkDeviceSize(uint32_t(slot(br.slot).neighborB[3]))*4;assert(aOffset!=bOffset);
        const auto haloSlot=live.plan().lookup(halo)->slot;
        const auto hOffset=VkDeviceSize(uint32_t(slot(haloSlot).neighborB[3]))*4;
        assert(hOffset!=aOffset&&hOffset!=bOffset);
        // Use a no-op frame to prove publication itself enqueues all mesh work.
        auto ready=live.record(cmd,0);assert(ready.empty());assert(run(core));
        const auto result=light.resultStreamCore(cmd,0,live).value();
        assert(result.bytes==65536);
        for(size_t i=0;i<16384;++i){const uint32_t w=i<8192?0xf3f3f3f3:0xf9f9f9f9;
            std::memcpy(B(result.buffer).memory->bytes.data()+result.offset+i*4,&w,4);}
        assert(live.recordResidentReadback(cmd,0,core[0]));
        ncopy=copies;
        assert(!light.publishStreamCore(cmd,0,live,newer)&&copies==ncopy);
        assert(!light.publishStreamCore(cmd,1,live,ready)&&copies==ncopy);
        watchResidentLight=true;lightBarriers.clear();
        assert(light.publishStreamCore(cmd,0,live,ready)&&copies==ncopy+2);
        assert(lightBarriers.size()==2);
        assert((lightBarriers[0].sa&VK_ACCESS_SHADER_READ_BIT)&&(lightBarriers[0].da&VK_ACCESS_TRANSFER_WRITE_BIT));
        assert((lightBarriers[1].sa&VK_ACCESS_TRANSFER_WRITE_BIT)&&(lightBarriers[1].da&VK_ACCESS_SHADER_READ_BIT));
        watchResidentLight=false;
        assert(ready.remeshSlots.size()==36&&std::is_sorted(ready.remeshSlots.begin(),ready.remeshSlots.end()));
        for(size_t i=0;i<8192;++i){assert(word(live.lights(),aOffset+i*4)==0xf3f3f3f3);assert(word(live.lights(),bOffset+i*4)==0xf9f9f9f9);}
        assert(!light.publishStreamCore(cmd,0,live,ready)&&copies==ncopy+2);
        for(size_t i=0;i<8192;++i)assert(word(live.lights(),hOffset+i*4)==(i?0xa7a7a7a7u:0xf1f1f1f1u));
        light.retire();
        auto lod=make(a,1,true);lod.lodRevision=1;live.submit({std::move(lod)});
        auto lodBatch=live.record(cmd,0);assert(lodBatch.uploads.size()==1&&!lodBatch.uploads[0].uploadVoxels);
        assert(word(live.lights(),aOffset)==0xf3f3f3f3);
        auto prePublish=live.takeResidentReadback(0);assert(prePublish&&!live.residentReadbackCurrent(*prePublish));
        assert(prePublish->words[2][0]==0xf1f1f1f1);
        assert(live.recordResidentReadback(cmd,0,core[0]));
        auto badLod=make(a,1,false);badLod.lodRevision=2;live.submit({std::move(badLod)});
        assert(live.record(cmd,0).empty()&&live.plan().lookup(a)->gpuBlockLightOwned);
        auto postPublish=live.takeResidentReadback(0);assert(postPublish&&live.residentReadbackCurrent(*postPublish));
        assert(postPublish->words[0]==std::vector<uint32_t>{0x00070007});
        assert(postPublish->words[1]==std::vector<uint32_t>{0});
        assert(postPublish->words[2].size()==8192 && std::all_of(postPublish->words[2].begin(),postPublish->words[2].end(),[](uint32_t w){return w==0xf3f3f3f3;}));
        // Handback restores CPU authority and inline storage. No stale light
        // publication may write that page even if the slot itself survives.
        assert(run(core));auto handbackPacket=make(a,1,false);handbackPacket.lightOwnershipRevision=3;
        handbackPacket.lodRevision=3;live.submit({handbackPacket});auto handback=live.record(cmd,0);
        assert(handback.uploads.size()==1&&handback.remeshSlots.size()==27);
        assert(!live.plan().lookup(a)->gpuBlockLightOwned&&(slot(ar.slot).origin[3]&kVoxelUniformLight));
        assert(!light.publishStreamCore(cmd,0,live,handback));light.retire();
        assert(run(core));ncopy=copies;assert(!light.publishStreamCore(cmd,0,live,handback)&&copies==ncopy);light.retire();
        auto denseCpu=make(a,3,false);denseCpu.lightOwnershipRevision=3;denseCpu.lightWords.resize(kChunkVoxels/4,0xf0f0f0f0);
        live.submit({std::move(denseCpu)});auto denseCpuBatch=live.record(cmd,0);
        assert(!(slot(ar.slot).origin[3]&kVoxelUniformLight)&&run(core));
        ncopy=copies;assert(!light.publishStreamCore(cmd,0,live,denseCpuBatch)&&copies==ncopy);light.retire();
        live.shutdown();
        // Force a two-page light budget; uniform GPU-owned uploads must not
        // silently alias inline data or consume an unallocated third page.
        fakeStorageLimit=65536;
        live.initialize(physical,device,{4,4,8,true,true,{1,1,1}});
        const ChunkKey c{0,1,1,1};
        live.submit({make(a,1,true),make(b,1,true),make(c,1,true)});
        auto pressure=live.record(cmd,0);assert(pressure.uploads.size()==2&&live.plan().pendingCount()==1);
        const auto key=pressure.uploads[0].packet.position;live.evict(key);
        auto refill=live.record(cmd,0);assert(refill.releases.size()==1&&refill.uploads.size()==1&&live.plan().pendingCount()==0);
        live.shutdown();fakeStorageLimit=128u*1024u*1024u;
    }
    std::cout<<"Stage51 owned light publication, dense admission, ordered core copies, remesh union, handback and stale rejection PASS (mock API)\n";
    // Stage52: schedule actual committed events, not producer intentions.
    {
        VulkanChunkStreamGpu live;live.initialize(physical,device,{80,80,160,true,true,{1,1,1}});
        ResidentLightScheduler scheduler;VulkanResidentLight light;
        const std::array<uint32_t,1> spv{0x07230203u};light.initialize(physical,device,36,spv);
        std::array<uint32_t,2048> bits{};std::array<uint8_t,65536> emission{};emission[7]=14;
        const ChunkKey a{0,-4,2,-7},b{0,-3,2,-7},missing{0,-5,1,-8},far{1,0,0,0};
        auto make=[](ChunkKey key,uint64_t version,bool owned){
            LiveChunkPacket p;p.position=key;p.chunkInstance=1;p.blocksVersion=p.meshVersion=version;
            p.blockWords={0x00070007};p.metaWords={0};p.lightWords={0xf1f1f1f1};p.gpuBlockLightOwned=owned;return p;
        };
        std::vector<LiveChunkPacket> packets;
        for(int x=-1;x<=2;++x)for(int y=-1;y<=1;++y)for(int z=-1;z<=1;++z){
            auto k=a;k.x+=x;k.y+=y;k.z+=z;if(k!=missing)packets.push_back(make(k,1,k==a||k==b));
        }
        packets.push_back(make(far,1,true));live.submit(std::move(packets));
        auto batch=live.record(cmd,0);scheduler.observe(batch);assert(scheduler.pendingCount()==3);
        auto selection=scheduler.select(live.plan(),26);assert(selection.keys.empty());
        selection=scheduler.select(live.plan());assert(selection.jobs==27&&selection.keys==std::vector<ChunkKey>{b});
        assert(!scheduler.record(cmd,0,live,batch,light,selection,std::span(bits).first(2047),emission));
        assert(scheduler.pendingCount()==3&&!light.busy());
        watchResidentLight=true;lightDispatches.clear();lightBarriers.clear();
        assert(scheduler.record(cmd,0,live,batch,light,selection,bits,emission));
        assert(lightDispatches.size()==14&&lightDispatches.front().jobCount==27&&scheduler.pendingCount()==2);
        assert(!scheduler.record(cmd,0,live,batch,light,selection,bits,emission));
        light.retire();watchResidentLight=false;
        assert(scheduler.recordedCurrent(live,batch,b)&&!scheduler.recordedCurrent(live,batch,a));
        scheduler.observe(batch);assert(scheduler.recordedCurrent(live,batch,b));
        // Completing the halo dirties only A, not B or another dimension.
        live.submit({make(missing,1,false)});auto ready=live.record(cmd,0);scheduler.observe(ready);
        selection=scheduler.select(live.plan());assert(selection.jobs==27&&selection.keys==std::vector<ChunkKey>{a});
        assert(scheduler.record(cmd,0,live,ready,light,selection,bits,emission));light.retire();
        assert(scheduler.pendingCount()==1);
        // CPU-owned shared halo edit marks both cores; 27-job limit admits
        // just one, 36 admits the union. LOD-only never dirties lighting.
        auto lod=make(a,1,true);lod.lodRevision=1;live.submit({std::move(lod)});
        auto lodBatch=live.record(cmd,0);scheduler.observe(lodBatch);assert(scheduler.pendingCount()==1);
        auto shared=a;shared.z++;
        live.submit({make(shared,2,false)});auto edited=live.record(cmd,0);scheduler.observe(edited);
        assert(scheduler.pendingCount()==3);
        assert(scheduler.select(live.plan(),27).keys.size()==1);
        selection=scheduler.select(live.plan(),36);assert(selection.jobs==36&&selection.keys.size()==2);
        watchResidentLight=true;lightDispatches.clear();lightBarriers.clear();
        assert(scheduler.record(cmd,0,live,edited,light,selection,bits,emission));
        assert(lightDispatches.size()==14&&lightDispatches.front().jobCount==36&&edited.remeshSlots.size()==36);
        assert(scheduler.pendingCount()==1);light.retire();watchResidentLight=false;
        // A missing halo is retained, not treated as air. Refill wakes it.
        live.evict(missing);auto evicted=live.record(cmd,0);scheduler.observe(evicted);
        assert(scheduler.pendingCount()==2&&scheduler.select(live.plan()).keys.empty());
        live.submit({make(missing,2,false)});auto refill=live.record(cmd,0);scheduler.observe(refill);
        selection=scheduler.select(live.plan());assert(selection.keys.size()==1&&selection.keys[0]==a);
        auto next=live.record(cmd,0);scheduler.observe(next);
        assert(!scheduler.record(cmd,0,live,next,light,selection,bits,emission)&&!light.busy());
        // Handback and eviction remove dirty owners without stale slot reuse.
        live.submit({make(a,2,false)});live.evict(far);auto handback=live.record(cmd,0);scheduler.observe(handback);
        assert(scheduler.pendingCount()==1); // B sees A's handback as a source update
        selection=scheduler.select(live.plan());assert(selection.keys==std::vector<ChunkKey>{b});
        assert(scheduler.record(cmd,0,live,handback,light,selection,bits,emission));light.retire();
        assert(!scheduler.pendingCount());
        // Stage147: light-only uploads do not invalidate neighbour emission.
        auto delta=make(shared,2,false);delta.lightOnly=true;delta.blockWords.clear();delta.metaWords.clear();
        delta.meshVersion=3;delta.lightWords={0x01010101};live.submit({delta});
        auto deltaBatch=live.record(cmd,0);scheduler.observe(deltaBatch);
        assert(deltaBatch.uploads.size()==1&&deltaBatch.uploads[0].packet.lightOnly);
        assert(!scheduler.pendingCount()&&scheduler.recordedCurrent(live,deltaBatch,b));
        auto ownDelta=make(b,1,true);ownDelta.lightOnly=true;ownDelta.blockWords.clear();ownDelta.metaWords.clear();
        ownDelta.meshVersion=2;ownDelta.lightWords={0x02020202};live.submit({ownDelta});
        auto ownDeltaBatch=live.record(cmd,0);scheduler.observe(ownDeltaBatch);
        assert(scheduler.pendingCount()==1&&!scheduler.recordedCurrent(live,ownDeltaBatch,b));
        selection=scheduler.select(live.plan());assert(selection.keys==std::vector<ChunkKey>{b});
        assert(scheduler.record(cmd,0,live,ownDeltaBatch,light,selection,bits,emission));light.retire();
        assert(!scheduler.pendingCount());
        auto skipped=live.record(cmd,0);auto latest=live.record(cmd,0);
        assert(!scheduler.recordedCurrent(live,skipped,b));scheduler.observe(latest);
        assert(scheduler.pendingCount()==1&&!scheduler.recordedCurrent(live,latest,b));
        selection=scheduler.select(live.plan());assert(selection.keys==std::vector<ChunkKey>{b});
        assert(scheduler.record(cmd,0,live,latest,light,selection,bits,emission));light.retire();
        live.evict(b);auto removed=live.record(cmd,0);scheduler.observe(removed);assert(!scheduler.pendingCount());
        scheduler.reset();live.shutdown();
    }
    std::cout<<"Stage52 scheduler committed events, incomplete halo, fair selection, budgets, publication, handback PASS (mock API)\n";
    // Stage62: exclusive world ownership is distinct from renderer mirror ownership.
    {
        using State=VulkanChunkStreamGpu::OwnershipState;
        VulkanChunkStreamGpu live;live.initialize(physical,device,{4,4,8,true,true,{1,1,1}});
        auto source=std::make_shared<OverlaySnapshot>();source->device=device;source->ticket=6201;source->ready=true;
        source->buffer.create(physical,device,98304,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        auto expected=packet(0,9000,1,0x00070007);expected.metaWords[15]=0x01020304;
        source->buffer.write(0,expected.blockWords.data(),65536);source->buffer.write(65536,expected.metaWords.data(),32768);
        auto ownedPacket=[&](int x){auto p=expected;p.position.x=x;p.blockWords.clear();p.metaWords.clear();
            p.overlayReference={source,6201};return p;};
        auto cpu=packet(3,1,1,0);live.submit({cpu});
        assert(!live.submitOwnedGpuSnapshot(ownedPacket(3)));
        live.record(cmd,0);assert(!live.submitOwnedGpuSnapshot(ownedPacket(3)));
        auto a=live.submitOwnedGpuSnapshot(ownedPacket(0));assert(a&&live.ownershipState(*a)==State::Queued);
        assert(!live.submitOwnedGpuSnapshot(ownedPacket(0))&&!live.beginOwnedPromotion(*a,1));
        auto cpuCollision=expected;cpuCollision.chunkInstance=UINT64_MAX;
        bool rejected=false;try{live.submit({packet(2,1,1,0),cpuCollision});}catch(const LodControlSourceMissing&){rejected=true;}
        assert(rejected&&live.plan().pendingCount()==1&&!live.plan().lookup({0,2,0,0}));
        live.evict(a->key);assert(live.plan().pendingCount()==1);
        live.record(cmd,1,false);assert(live.ownershipState(*a)==State::Queued);
        live.record(cmd,0);assert(live.ownershipState(*a)==State::Resident);
        auto b=live.submitOwnedGpuSnapshot(ownedPacket(1));auto c=live.submitOwnedGpuSnapshot(ownedPacket(2));assert(b&&c);
        live.record(cmd,1);
        auto t=live.beginOwnedPromotion(*a,100),u=live.beginOwnedPromotion(*b,101);
        assert(t&&u&&live.ownershipState(*a)==State::Promoting);
        assert(!live.beginOwnedPromotion(*c,102)&&!live.discardOwnedChunk(*a));
        assert(live.cancelOwnedPromotion(*u)&&!live.cancelOwnedPromotion(*u));
        assert(!live.beginOwnedPromotion(*c,100));
        auto v=live.beginOwnedPromotion(*c,103);assert(v&&live.cancelOwnedPromotion(*v));
        assert(live.recordOwnedReadback(cmd,1,*t)&&!live.recordOwnedReadback(cmd,1,*t));
        assert(!live.takeResidentReadback(1));live.record(cmd,1);
        assert(!live.takeOwnedReadback(1,*u)&&!live.takeOwnedReadback(0,*t));
        auto read=live.takeOwnedReadback(1,*t);assert(read&&read->words[0]==expected.blockWords&&
            read->words[1]==expected.metaWords&&read->words[2]==expected.lightWords);
        int calls=0;
        assert(!live.commitOwnedPromotion(*t,*read,[&]{++calls;return false;})&&calls==1);
        assert(live.ownershipState(*a)==State::Promoting);
        assert(live.cancelOwnedPromotion(*t));auto retry=live.beginOwnedPromotion(*a,104);assert(retry&&*retry!=*t);
        assert(!live.commitOwnedPromotion(*t,*read,[&]{++calls;return true;})&&calls==1);
        assert(!live.commitOwnedPromotion(*retry,*read,[&]{++calls;return true;})&&calls==1);
        assert(live.recordOwnedReadback(cmd,1,*retry));live.record(cmd,1);
        assert(!live.takeOwnedReadback(1,*t));
        auto retryRead=live.takeOwnedReadback(1,*retry);assert(retryRead&&retryRead->promotion==*retry);
        assert(live.commitOwnedPromotion(*retry,*retryRead,[&]{++calls;return true;})&&calls==2);
        assert(live.ownershipState(*a)==State::Retiring);
        auto replacement=expected;replacement.chunkInstance=2;
        rejected=false;try{live.submit({replacement});}catch(const LodControlSourceMissing&){rejected=true;}
        assert(rejected);live.evict(a->key);
        auto retired=live.record(cmd,0,false);assert(retired.releases.size()==1&&!live.ownershipState(*a));
        live.submit({replacement});live.record(cmd,1);assert(live.plan().lookup(a->key)->instance==2);
        assert(!live.residentReadbackCurrent(*read)&&!live.discardOwnedChunk(*a));
        assert(live.discardOwnedChunk(*b)&&!live.discardOwnedChunk(*b));
        live.record(cmd,0);assert(!live.ownershipState(*b)&&!live.plan().lookup(b->key));
        auto pending=live.submitOwnedGpuSnapshot(ownedPacket(1));assert(pending&&*pending!=*b);
        assert(live.discardOwnedChunk(*pending));live.record(cmd,1,false);
        assert(!live.ownershipState(*pending)&&!live.plan().hasPendingChange(pending->key));
        live.shutdown();live.initialize(physical,device,{4,4,8,true,true,{1,1,1}});
        auto fresh=live.submitOwnedGpuSnapshot(ownedPacket(0));assert(fresh&&*fresh!=*a&&!live.ownershipState(*a));
        live.shutdown();source.reset();
    }
    std::cout<<"Stage62 exclusive ownership, producer exclusion, promotion tickets, retry and retirement PASS (mock API)\n";
    // Stage70: actual ceiling extension and exclusive world sky publication.
    {
        VulkanChunkStreamGpu live;live.initialize(physical,device,{50,50,100,true,true,{1,1,1}});
        VulkanResidentLight sky,block,small;
        const std::array<uint32_t,1> spv{0x07230203u};
        sky.initialize(physical,device,45,spv);block.initialize(physical,device,27,spv);small.initialize(physical,device,35,spv);
        std::array<uint32_t,2048> bits{};bits[0]=1;
        std::array<uint8_t,65536> emission{};emission[7]=14;
        const ChunkKey center{0,-4,2,-7},gap{0,-5,3,-8};
        auto make=[](ChunkKey k){LiveChunkPacket p;p.position=k;p.chunkInstance=1;
            p.blocksVersion=p.meshVersion=1;p.blockWords={0};p.metaWords={0};p.lightWords={0x07070707};return p;};
        auto source=std::make_shared<OverlaySnapshot>();source->device=device;source->ticket=7001;source->ready=true;
        source->buffer.create(physical,device,98304,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        std::vector<uint32_t> zero(24576);source->buffer.write(0,zero.data(),zero.size()*4);
        auto owned=make(center);owned.blockWords.clear();owned.overlayReference={source,7001};owned.gpuBlockLightOwned=true;
        auto owner=live.submitOwnedGpuSnapshot(std::move(owned));assert(owner);
        std::vector<LiveChunkPacket> packets;
        for(int x=-1;x<=1;++x)for(int y=-1;y<=3;++y)for(int z=-1;z<=1;++z){
            auto k=center;k.x+=x;k.y+=y;k.z+=z;if(k==center||k==gap)continue;
            packets.push_back(make(k));
        }
        live.submit(std::move(packets));auto batch=live.record(cmd,0);assert(batch.uploads.size()==44);
        auto r=live.plan().lookup(center).value();std::array<ChunkHandle,1> core{{{r.slot,r.generation}}};
        const auto run=[&](VulkanResidentLight& light,VulkanResidentLight::SkyBoundary b){return light.recordStreamSkyCore(cmd,0,live,core,bits,b);};
        assert(!run(sky,{true,5}));assert(!run(sky,{true,1}));
        assert(run(sky,{true,2}));sky.retire();
        std::array<uint32_t,32> preview{};preview[0]=1;
        watchResidentLight=true;lightDispatches.clear();
        assert(sky.recordStreamPreviewSky(cmd,0,live,core[0],bits,preview));
        assert(lightDispatches.size()==15&&lightDispatches[0].jobCount==1);
        assert(sky.publishStreamCore(cmd,0,live,batch));sky.retire();watchResidentLight=false;
        assert(block.recordStreamPreviewBlock(cmd,0,live,core[0],bits,emission));
        assert(block.publishStreamCore(cmd,0,live,batch));block.retire();
        // Stage140: resident halo previews publish only core; unknown tops closed.
        const auto upperKey=ChunkKey{center.dimension,center.x,center.y+1,center.z};
        const auto upper=live.plan().lookup(upperKey).value();
        const auto stale=live.plan().lookup(ChunkKey{center.dimension,center.x+1,center.y+1,center.z}).value();
        const std::array<VulkanResidentLight::PreviewSkySeed,3> seeds{{
            {{upper.slot,upper.generation},preview},{{core[0].slot,core[0].generation},preview},
            {{stale.slot,stale.generation+1},preview}}};
        watchResidentLight=true;lightDispatches.clear();
        assert(sky.recordStreamPreviewSky(cmd,0,live,core[0],bits,preview,seeds,true));
        assert(lightDispatches.size()==15&&lightDispatches[0].jobCount==26);
        const auto& previewDs=descriptorState.at(residentLightSet)->bindings;
        const auto previewTable=previewDs.at(1).buffer,previewMasks=previewDs.at(3).buffer;
        size_t seeded=0;
        for(size_t i=0;i<26;++i){const bool top=word(previewTable,i*64+16)==uint32_t(center.x*32)&&word(previewTable,i*64+20)==uint32_t((center.y+1)*32)&&word(previewTable,i*64+24)==uint32_t(center.z*32);
            seeded+=top;for(size_t j=0;j<32;++j)assert(word(previewMasks,(2048+i*32+j)*4)==(top?preview[j]:0));}
        assert(seeded==1);assert(sky.resultStreamCore(cmd,0,live)->bytes==32768);
        assert(sky.publishStreamCore(cmd,0,live,batch));sky.retire();lightDispatches.clear();
        assert(block.recordStreamPreviewBlock(cmd,0,live,core[0],bits,emission,true));
        assert(lightDispatches[0].jobCount==26&&block.publishStreamCore(cmd,0,live,batch));block.retire();watchResidentLight=false;
        // Stage141: extend contiguous upper chains, never bridge the gap.
        watchResidentLight=true;lightDispatches.clear();
        assert(sky.recordStreamPreviewSky(cmd,0,live,core[0],bits,preview,seeds,true,5));
        assert(lightDispatches.size()==15&&lightDispatches[0].jobCount==42);
        const auto extendedTable=descriptorState.at(residentLightSet)->bindings.at(1).buffer;
        const auto extendedMasks=descriptorState.at(residentLightSet)->bindings.at(3).buffer;
        size_t ceilingSeeds=0;
        for(size_t i=0;i<42;++i){
            const auto x=int32_t(word(extendedTable,i*64+16)),y=int32_t(word(extendedTable,i*64+20)),z=int32_t(word(extendedTable,i*64+24));
            assert(!(x==gap.x*32&&z==gap.z*32&&y>=gap.y*32));
            ceilingSeeds+=y==160;
            for(size_t j=0;j<32;++j)assert(word(extendedMasks,(2048+i*32+j)*4)==(y==160?UINT32_MAX:0));
        }
        assert(ceilingSeeds==8&&sky.resultStreamCore(cmd,0,live)->bytes==32768);
        assert(sky.publishStreamCore(cmd,0,live,batch));sky.retire();lightDispatches.clear();
        assert(small.recordStreamPreviewSky(cmd,0,live,core[0],bits,preview,seeds,true,5));
        assert(lightDispatches[0].jobCount==35);
        assert(small.publishStreamCore(cmd,0,live,batch));small.retire();lightDispatches.clear();
        assert(sky.recordStreamPreviewSky(cmd,0,live,core[0],bits,preview,seeds,true,2));
        assert(lightDispatches[0].jobCount==18);
        const auto capTable=descriptorState.at(residentLightSet)->bindings.at(1).buffer;
        const auto capMasks=descriptorState.at(residentLightSet)->bindings.at(3).buffer;
        for(size_t i=0;i<18;++i){const auto y=int32_t(word(capTable,i*64+20));assert(y<=64);
            for(size_t j=0;j<32;++j)assert(word(capMasks,(2048+i*32+j)*4)==(y==64?UINT32_MAX:0));}
        assert(sky.publishStreamCore(cmd,0,live,batch));sky.retire();watchResidentLight=false;
        ResidentSkylightScheduler exact;exact.observe(live,batch);exact.setBoundary(0,{true,5});
        assert(!exact.recordedCurrent(live,batch,center));
        live.submit({make(gap)});batch=live.record(cmd,0);assert(batch.uploads.size()==1);
        assert(!run(small,{true,5}));assert(!run(sky,{true,6}));
        watchResidentLight=true;lightDispatches.clear();lightBarriers.clear();
        assert(run(sky,{true,5}));assert(lightDispatches.size()==15&&lightDispatches[0].jobCount==45);
        const auto& ds=descriptorState.at(residentLightSet)->bindings;
        const auto table=ds.at(1).buffer,masks=ds.at(3).buffer;
        size_t topCount=0;
        for(size_t i=0;i<45;++i){const bool top=word(table,i*64+20)==160;
            topCount+=top;for(size_t j=0;j<32;++j)assert(word(masks,(2048+i*32+j)*4)==(top?UINT32_MAX:0));}
        assert(topCount==9&&word(table,20)==64);watchResidentLight=false;
        auto sr=sky.resultStreamCore(cmd,0,live);assert(sr&&sr->bytes==32768);
        assert(block.recordStreamCore(cmd,0,live,core,bits,emission));
        assert(block.resultStreamCore(cmd,0,live));
        uint32_t value=0xa7a7a7a7;
        for(size_t i=0;i<8192;++i)std::memcpy(B(sr->buffer).memory->bytes.data()+sr->offset+i*4,&value,4);
        batch.remeshSlots.clear();auto nc=copies;
        assert(sky.publishStreamCore(cmd,0,live,batch)&&copies==nc+1&&batch.remeshSlots.size()==27);
        assert(!block.resultStreamCore(cmd,0,live)&&!block.publishStreamCore(cmd,0,live,batch));
        assert(!sky.publishStreamCore(cmd,0,live,batch));
        auto dest=VkDeviceSize(word(live.opaqueSlots(),r.slot*64+60))*4;
        for(size_t i=0;i<8192;++i)assert(word(live.lights(),dest+i*4)==value);
        sky.retire();block.retire();watchResidentLight=true;lightDispatches.clear();
        assert(run(sky,{false,0}));assert(lightDispatches.size()==1&&lightDispatches[0].jobCount==1);watchResidentLight=false;
        auto promotion=live.beginOwnedPromotion(*owner,700);assert(promotion);
        nc=copies;assert(!sky.publishStreamCore(cmd,0,live,batch)&&copies==nc);
        assert(live.cancelOwnedPromotion(*promotion));sky.retire();
        assert(run(sky,{true,5}));assert(live.discardOwnedChunk(*owner));
        assert(!sky.publishStreamCore(cmd,0,live,batch));sky.retire();
        live.record(cmd,0);auto cpu=make(center);cpu.gpuBlockLightOwned=true;live.submit({cpu});batch=live.record(cmd,0);
        r=live.plan().lookup(center).value();core[0]={r.slot,r.generation};
        assert(run(sky,{true,5}));nc=copies;assert(!sky.publishStreamCore(cmd,0,live,batch)&&copies==nc);
        sky.retire();live.shutdown();source.reset();
    }
    // Stage127: forced retirement cannot be superseded by a lower CPU instance.
    {
        VulkanChunkStreamPlan plan(4,8);
        LiveChunkPacket old;old.position={1,2,3,4};old.chunkInstance=100;old.blocksVersion=old.meshVersion=1;
        old.blockWords={0};old.metaWords={0};old.lightWords={0};
        plan.submit({old});assert(plan.take(4).uploads.size()==1);
        plan.retireForReplacement(old.position);old.chunkInstance=1;plan.submit({old});
        auto next=plan.take(4);assert(next.releases.size()==1&&next.uploads.size()==1);
        assert(plan.lookup(old.position)->instance==1);
    }
    std::cout<<"Stage70 sky ceiling, gap, publication and packed-channel guard PASS (mock API)\n";
    // Stage71: vertical dependencies from committed resident events.
    {
        VulkanChunkStreamGpu live;live.initialize(physical,device,{100,100,200,true,true,{1,1,1}});
        ResidentSkylightScheduler scheduler;VulkanResidentLight sky;
        const std::array<uint32_t,1> spv{0x07230203u};sky.initialize(physical,device,72,spv);
        std::array<uint32_t,2048> bits{};bits[0]=1;
        const ChunkKey a{0,-4,2,-7},b{0,-4,4,-7},gap{0,-5,1,-8},roof{0,-5,7,-8},mirror{0,-3,3,-7};
        auto make=[](ChunkKey k,uint64_t version=1){LiveChunkPacket p;p.position=k;p.chunkInstance=1;
            p.blocksVersion=p.meshVersion=version;p.blockWords={0};p.metaWords={0};p.lightWords={0x07070707};return p;};
        auto source=std::make_shared<OverlaySnapshot>();source->device=device;source->ticket=7101;source->ready=true;
        source->buffer.create(physical,device,98304,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        std::vector<uint32_t> zero(24576);source->buffer.write(0,zero.data(),zero.size()*4);
        auto own=[&](ChunkKey key){auto p=make(key);p.blockWords.clear();p.overlayReference={source,7101};
            p.gpuBlockLightOwned=true;return live.submitOwnedGpuSnapshot(std::move(p));};
        auto oa=own(a),ob=own(b);assert(oa&&ob);
        std::vector<LiveChunkPacket> packets;
        for(int x=-1;x<=1;++x)for(int y=1;y<=7;++y)for(int z=-1;z<=1;++z){
            auto k=a;k.x+=x;k.y=y;k.z+=z;if(k==a||k==b||k==gap)continue;
            auto p=make(k);p.gpuBlockLightOwned=k==mirror;packets.push_back(std::move(p));
        }
        live.submit(std::move(packets));auto batch=live.record(cmd,0);
        assert(scheduler.observe(live,batch)&&scheduler.pendingCount()==2);
        assert(scheduler.select(live).keys.empty());
        scheduler.setBoundary(0,{true,7});assert(scheduler.select(live,44).keys.empty());
        auto selection=scheduler.select(live);assert(selection.jobs==45&&selection.keys==std::vector<ChunkKey>{b});
        assert(!scheduler.record(cmd,0,live,batch,sky,selection,std::span(bits).first(2047)));
        assert(scheduler.pendingCount()==2&&!sky.busy());
        watchResidentLight=true;lightDispatches.clear();
        assert(scheduler.record(cmd,0,live,batch,sky,selection,bits));
        assert(lightDispatches.size()==15&&lightDispatches.front().jobCount==45);watchResidentLight=false;
        assert(scheduler.pendingCount()==1&&scheduler.pending(a)&&!scheduler.pending(b));
        assert(scheduler.recordedCurrent(live,batch,b)&&!scheduler.recordedCurrent(live,batch,a)&&
               !scheduler.recordedCurrent(live,batch,mirror));sky.retire();
        auto early=live.beginOwnedPromotion(*ob,711);assert(early);
        assert(scheduler.recordedCurrent(live,batch,b));
        assert(live.cancelOwnedPromotion(*early));
        live.submit({make(gap)});batch=live.record(cmd,0);assert(scheduler.observe(live,batch));
        selection=scheduler.select(live);assert(selection.jobs==63&&selection.keys==std::vector<ChunkKey>{a});
        assert(scheduler.record(cmd,0,live,batch,sky,selection,bits));sky.retire();assert(!scheduler.pendingCount());
        assert(scheduler.observe(live,batch)&&!scheduler.pendingCount());
        auto changed=make(roof,2);changed.blockWords={0x00070007};live.submit({changed});
        batch=live.record(cmd,0);assert(scheduler.observe(live,batch)&&scheduler.pendingCount()==2);
        selection=scheduler.select(live,62);assert(selection.jobs==45&&selection.keys==std::vector<ChunkKey>{b});
        selection=scheduler.select(live,63);assert(selection.jobs==63&&selection.keys.size()==2);
        auto promotion=live.beginOwnedPromotion(*oa,710);assert(promotion);
        assert(!scheduler.record(cmd,0,live,batch,sky,selection,bits)&&!sky.busy());
        selection=scheduler.select(live);assert(selection.keys==std::vector<ChunkKey>{b});
        assert(live.cancelOwnedPromotion(*promotion));selection=scheduler.select(live);assert(selection.keys.size()==2);
        watchResidentLight=true;lightDispatches.clear();
        assert(scheduler.record(cmd,0,live,batch,sky,selection,bits));
        assert(lightDispatches.size()==15&&lightDispatches.front().jobCount==63);watchResidentLight=false;
        sky.retire();assert(!scheduler.pendingCount());
        auto lod=changed;lod.lodRevision=1;live.submit({lod});batch=live.record(cmd,0);
        assert(scheduler.observe(live,batch)&&!scheduler.pendingCount());
        auto light=lod;light.lightOnly=true;light.blockWords.clear();light.metaWords.clear();light.meshVersion=3;
        light.lightWords={0x17171717};live.submit({light});batch=live.record(cmd,0);
        assert(scheduler.observe(live,batch)&&!scheduler.pendingCount());
        live.evict(roof);batch=live.record(cmd,0);assert(scheduler.observe(live,batch)&&scheduler.pendingCount()==2);
        assert(scheduler.select(live).keys.empty());live.submit({make(roof,3)});batch=live.record(cmd,0);
        assert(scheduler.observe(live,batch));selection=scheduler.select(live);assert(selection.keys.size()==2);
        scheduler.setBoundary(0,{false,0});assert(!scheduler.record(cmd,0,live,batch,sky,selection,bits));
        selection=scheduler.select(live);assert(selection.jobs==2&&selection.keys.size()==2);
        watchResidentLight=true;lightDispatches.clear();assert(scheduler.record(cmd,0,live,batch,sky,selection,bits));
        assert(lightDispatches.size()==1);watchResidentLight=false;sky.retire();assert(!scheduler.pendingCount());
        // Stage147: no-sky certificates survive neighbour geometry and eviction.
        live.submit({make(roof,4)});batch=live.record(cmd,0);assert(scheduler.observe(live,batch));
        assert(!scheduler.pendingCount()&&scheduler.recordedCurrent(live,batch,a)&&scheduler.recordedCurrent(live,batch,b));
        live.evict(roof);batch=live.record(cmd,0);assert(scheduler.observe(live,batch)&&!scheduler.pendingCount());
        live.submit({make(roof,5)});batch=live.record(cmd,0);assert(scheduler.observe(live,batch)&&!scheduler.pendingCount());
        scheduler.setBoundary(0,{true,7});selection=scheduler.select(live);assert(selection.keys.size()==2);
        auto old=batch;live.record(cmd,0);batch=live.record(cmd,0);
        assert(!scheduler.recordedCurrent(live,old,a)&&!scheduler.recordedCurrent(live,batch,a));
        assert(!scheduler.observe(live,old));assert(scheduler.observe(live,batch)&&scheduler.pendingCount()==2);
        assert(!scheduler.record(cmd,0,live,batch,sky,selection,bits));
        selection=scheduler.select(live);assert(scheduler.record(cmd,0,live,batch,sky,selection,bits));sky.retire();
        assert(live.discardOwnedChunk(*oa));batch=live.record(cmd,0);
        assert(scheduler.observe(live,batch)&&!scheduler.pendingCount());
        scheduler.invalidateDimension(0);assert(scheduler.pendingCount()==1);
        selection=scheduler.select(live);assert(selection.keys==std::vector<ChunkKey>{b});
        scheduler.reset();assert(!scheduler.record(cmd,0,live,batch,sky,selection,bits));
        live.shutdown();source.reset();
    }
    std::cout<<"Stage71 skylight scheduler, vertical invalidation, union budget, ownership and missed events PASS (mock API)\n";
    // Stage142: exact world-edge halo and isolated no-sky certificates.
    for(int edge:{worldLightBounds.low,worldLightBounds.high}){
        VulkanChunkStreamGpu live;live.initialize(physical,device,{32,32,64,true,true,{1,1,1}});
        VulkanResidentLight sky,block;
        const std::array<uint32_t,1> spv{0x07230203u};
        sky.initialize(physical,device,27,spv);block.initialize(physical,device,27,spv);
        ResidentSkylightScheduler ss;ResidentLightScheduler bs;
        std::array<uint32_t,2048> bits{};bits[0]=1;
        std::array<uint8_t,65536> emission{};emission[7]=14;
        const ChunkKey center{0,0,edge,0},gap{0,1,edge,1};
        auto make=[](ChunkKey k){LiveChunkPacket p;p.position=k;p.chunkInstance=1;
            p.blocksVersion=p.meshVersion=1;p.blockWords={0};p.metaWords={0};p.lightWords={0x07070707};return p;};
        auto source=std::make_shared<OverlaySnapshot>();source->device=device;source->ticket=14201;source->ready=true;
        source->buffer.create(physical,device,98304,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        std::vector<uint32_t> zero(24576);source->buffer.write(0,zero.data(),zero.size()*4);
        auto owned=make(center);owned.blockWords.clear();owned.overlayReference={source,14201};owned.gpuBlockLightOwned=true;
        assert(live.submitOwnedGpuSnapshot(std::move(owned)));
        auto batch=live.record(cmd,0);assert(batch.uploads.size()==1);
        ss.observe(live,batch);ss.setBoundary(0,{false,0});bs.observe(batch);
        auto sel=ss.select(live,1);assert(sel.jobs==1&&sel.keys==std::vector<ChunkKey>{center});
        watchResidentLight=true;lightDispatches.clear();
        assert(ss.record(cmd,0,live,batch,sky,sel,bits));
        assert(lightDispatches.size()==1&&lightDispatches[0].jobCount==1);
        assert(ss.recordedCurrent(live,batch,center));sky.retire();watchResidentLight=false;
        assert(bs.select(live.plan()).keys.empty());
        ss.setBoundary(0,{true,worldLightBounds.high});assert(ss.select(live).keys.empty());
        std::vector<LiveChunkPacket> packets;
        for(int x=-1;x<=1;++x)for(int y=-1;y<=1;++y)for(int z=-1;z<=1;++z){
            auto k=center;k.x+=x;k.y+=y;k.z+=z;
            if(!worldLightBounds.contains(k.y)||k==center||k==gap)continue;
            packets.push_back(make(k));
        }
        live.submit(std::move(packets));batch=live.record(cmd,0);ss.observe(live,batch);bs.observe(batch);
        assert(bs.select(live.plan()).keys.empty()&&ss.select(live).keys.empty());
        live.submit({make(gap)});batch=live.record(cmd,0);ss.observe(live,batch);bs.observe(batch);
        assert(bs.select(live.plan(),17).keys.empty());
        auto bsel=bs.select(live.plan(),18);assert(bsel.jobs==18&&bsel.keys==std::vector<ChunkKey>{center});
        watchResidentLight=true;lightDispatches.clear();
        assert(bs.record(cmd,0,live,batch,block,bsel,bits,emission));
        assert(lightDispatches[0].jobCount==18&&bs.recordedCurrent(live,batch,center));block.retire();
        sel=ss.select(live);
        if(edge==worldLightBounds.high){
            assert(sel.jobs==18&&sel.keys==std::vector<ChunkKey>{center});lightDispatches.clear();
            assert(ss.record(cmd,0,live,batch,sky,sel,bits));
            assert(lightDispatches.size()==15&&lightDispatches[0].jobCount==18);
            const auto table=descriptorState.at(residentLightSet)->bindings.at(1).buffer;
            const auto masks=descriptorState.at(residentLightSet)->bindings.at(3).buffer;
            for(size_t i=0;i<18;++i)for(size_t j=0;j<32;++j)
                assert(word(masks,(2048+i*32+j)*4)==(int32_t(word(table,i*64+20))==edge*32?UINT32_MAX:0));
            assert(ss.recordedCurrent(live,batch,center));sky.retire();
        }else assert(sel.keys.empty()); // upper in-world chain still unknown
        watchResidentLight=false;live.shutdown();source.reset();
    }
    std::cout<<"Stage142 physical world edges and isolated no-sky exact certificates PASS (mock API)\n";
    // Stage143: opaque roofs certify the direct-sky boundary without the
    // missing layers above. The mandatory halo remains strict.
    {
        VulkanChunkStreamGpu live;live.initialize(physical,device,{64,64,128,true,true,{1,1,1}});
        VulkanResidentLight sky;ResidentSkylightScheduler ss;
        const std::array<uint32_t,1> spv{0x07230203u};sky.initialize(physical,device,32,spv);
        std::array<uint32_t,2048> bits{};bits[0]=1;
        const ChunkKey center{0,-10,0,-20},roof{0,-9,1,-19},gap{0,-11,-1,-21};
        auto make=[](ChunkKey k,uint64_t version=1,uint32_t word=0){LiveChunkPacket p;p.position=k;p.chunkInstance=1;
            p.blocksVersion=p.meshVersion=version;p.blockWords={word};p.metaWords={0};p.lightWords={0x07070707};return p;};
        auto source=std::make_shared<OverlaySnapshot>();source->device=device;source->ticket=14301;source->ready=true;
        source->buffer.create(physical,device,98304,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        std::vector<uint32_t> zero(24576);source->buffer.write(0,zero.data(),zero.size()*4);
        auto owned=make(center);owned.blockWords.clear();owned.overlayReference={source,14301};owned.gpuBlockLightOwned=true;
        assert(live.submitOwnedGpuSnapshot(std::move(owned)));
        std::vector<LiveChunkPacket> packets;
        for(int x=-1;x<=1;++x)for(int y=-1;y<=1;++y)for(int z=-1;z<=1;++z){
            auto k=center;k.x+=x;k.y+=y;k.z+=z;if(k==center||k==gap)continue;
            packets.push_back(make(k,1,y==1?0x00070007u:0u));
        }
        live.submit(std::move(packets));auto batch=live.record(cmd,0);ss.observe(live,batch);
        ss.setBoundary(0,{true,4});
        auto select=[&](uint32_t budget=32){return ss.select(live,budget,32,bits);};
        assert(live.uniformSkyOccluder(roof,bits)&&!live.uniformSkyOccluder(center,bits));
        assert(select().keys.empty()); // missing mandatory halo below
        live.submit({make(gap)});batch=live.record(cmd,0);ss.observe(live,batch);
        assert(ss.select(live,32).keys.empty()&&select(26).keys.empty());
        auto sel=select();assert(sel.jobs==27&&sel.keys==std::vector<ChunkKey>{center});
        watchResidentLight=true;lightDispatches.clear();assert(ss.record(cmd,0,live,batch,sky,sel,bits));
        assert(lightDispatches.size()==1&&lightDispatches[0].jobCount==27&&ss.recordedCurrent(live,batch,center));sky.retire();
        // Stage144: dense air-filled page with an opaque, mixed-ID top plane.
        auto dense=make(roof,2);dense.blockWords.resize(16384,0);
        std::fill(dense.blockWords.end()-512,dense.blockWords.end(),0x00080007u);
        live.submit({dense});batch=live.record(cmd,0);ss.observe(live,batch);
        assert(!live.uniformSkyOccluder(roof,bits)&&live.skyOccluder(roof,bits));
        auto changedBits=bits;changedBits[0]|=1u<<8;
        assert(!live.skyOccluder(roof,changedBits)&&ss.select(live,32,32,changedBits).keys.empty());
        sel=select();assert(sel.jobs==27&&sel.keys.size()==1);lightDispatches.clear();
        assert(ss.record(cmd,0,live,batch,sky,sel,bits));
        assert(lightDispatches.size()==1&&lightDispatches[0].jobCount==27);sky.retire();
        auto light=dense;light.lightOnly=true;light.blockWords.clear();light.metaWords.clear();
        light.meshVersion=3;light.lightWords={0x17171717};live.submit({light});batch=live.record(cmd,0);ss.observe(live,batch);
        assert(live.skyOccluder(roof,bits)&&!ss.pending(center));
        // A hole in the last word revokes the certificate.
        dense.blocksVersion=dense.meshVersion=4;dense.blockWords.back()=0;
        live.submit({dense});batch=live.record(cmd,0);ss.observe(live,batch);
        assert(!live.skyOccluder(roof,bits)&&ss.pending(center)&&select().keys.empty());
        // A palette overflow is conservative even if all IDs are opaque.
        dense.blocksVersion=dense.meshVersion=5;dense.blockWords.back()=0x00080007u;
        for(uint32_t i=0;i<9;++i)dense.blockWords[16384-512+i]=(20u+i)*65537u;
        live.submit({dense});batch=live.record(cmd,0);ss.observe(live,batch);
        assert(!live.skyOccluder(roof,bits)&&select().keys.empty());
        // CPU fallback data must never certify a different GPU overlay copy.
        dense.blocksVersion=dense.meshVersion=6;
        std::fill(dense.blockWords.end()-512,dense.blockWords.end(),0x00080007u);
        dense.overlayReference={source,14301};live.submit({dense});batch=live.record(cmd,0);ss.observe(live,batch);
        assert(!live.skyOccluder(roof,bits)&&select().keys.empty());
        // Stage145: immutable GPU snapshot carries its actual top-plane palette.
        auto certifiedRoof=std::make_shared<OverlaySnapshot>();certifiedRoof->device=device;
        certifiedRoof->ticket=14501;certifiedRoof->ready=true;
        certifiedRoof->buffer.create(physical,device,98304,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        std::vector<uint32_t> certifiedWords(24576);std::fill(certifiedWords.begin()+15872,certifiedWords.begin()+16384,0x00080007u);
        certifiedRoof->buffer.write(0,certifiedWords.data(),certifiedWords.size()*4);
        certifiedRoof->skyRoof=SkyRoofPalette::fromBlocks(std::span<const uint32_t>(certifiedWords.data(),16384));
        dense.blocksVersion=dense.meshVersion=7;dense.overlayReference={certifiedRoof,14501};
        std::fill(dense.blockWords.begin(),dense.blockWords.end(),0u); // deliberately different fallback
        live.submit({dense});batch=live.record(cmd,0);ss.observe(live,batch);
        assert(live.skyOccluder(roof,bits)&&!live.skyOccluder(roof,changedBits)&&select().jobs==27);
        // Source-only strong lease has the same certificate; no fallback bytes.
        auto gpuKey=roof;gpuKey.x+=8;
        auto gpuRoof=make(gpuKey);gpuRoof.blockWords.clear();gpuRoof.overlayReference={certifiedRoof,14501};
        assert(live.submitGpuSnapshot(std::move(gpuRoof)));batch=live.record(cmd,0);ss.observe(live,batch);
        assert(live.skyOccluder(gpuKey,bits)&&!live.skyOccluder(gpuKey,changedBits));
        live.evict(gpuKey);batch=live.record(cmd,0);ss.observe(live,batch);assert(!live.skyOccluder(gpuKey,bits));
        // A mixed packed word with a transparent ID cannot prove occlusion.
        live.submit({make(roof,8,0x00070000u)});batch=live.record(cmd,0);ss.observe(live,batch);
        assert(!live.skyOccluder(roof,bits)&&select().keys.empty());
        // One open column must reach the ceiling; all other roofs stop early.
        packets.clear();packets.push_back(make(roof,9));for(int y=2;y<=4;++y){auto k=roof;k.y=y;packets.push_back(make(k));}
        live.submit(std::move(packets));batch=live.record(cmd,0);ss.observe(live,batch);
        assert(select(29).keys.empty());sel=select();assert(sel.jobs==30&&sel.keys.size()==1);
        lightDispatches.clear();assert(ss.record(cmd,0,live,batch,sky,sel,bits));
        assert(lightDispatches.size()==15&&lightDispatches[0].jobCount==30);
        const auto table=descriptorState.at(residentLightSet)->bindings.at(1).buffer;
        const auto masks=descriptorState.at(residentLightSet)->bindings.at(3).buffer;
        size_t ceiling=0;
        for(size_t i=0;i<30;++i){bool top=int32_t(word(table,i*64+20))==128;ceiling+=top;
            for(size_t j=0;j<32;++j)assert(word(masks,(2048+i*32+j)*4)==(top?UINT32_MAX:0));}
        assert(ceiling==1);sky.retire();
        // Stage146: previews spend their vertical budget on open chains.
        packets.clear();auto other=roof;--other.x;
        for(int y=2;y<=4;++y){auto k=other;k.y=y;packets.push_back(make(k));}
        live.submit(std::move(packets));batch=live.record(cmd,0);ss.observe(live,batch);
        std::array<uint32_t,32> preview{};preview.fill(UINT32_MAX);
        const auto core=live.plan().lookup(center).value();const ChunkHandle handle{core.slot,core.generation};
        lightDispatches.clear();assert(sky.recordStreamPreviewSky(cmd,0,live,handle,bits,preview,{},true,4));
        assert(lightDispatches.size()==15&&lightDispatches[0].jobCount==30);
        assert(sky.publishStreamCore(cmd,0,live,batch));sky.retire();
        // An opaque physical ceiling cannot emit direct sky even with an open mask.
        auto top=roof;top.y=4;live.submit({make(top,2,0x00080007u)});
        batch=live.record(cmd,0);ss.observe(live,batch);lightDispatches.clear();
        assert(sky.recordStreamPreviewSky(cmd,0,live,handle,bits,preview,{},true,4));
        assert(lightDispatches.size()==1&&lightDispatches[0].jobCount==30);sky.retire();
        sel=select();assert(sel.jobs==30);lightDispatches.clear();
        assert(ss.record(cmd,0,live,batch,sky,sel,bits));
        assert(lightDispatches.size()==1&&lightDispatches[0].jobCount==30);sky.retire();
        // Terminate a chain at the first newly captured opaque plane.
        auto mid=roof;mid.y=2;live.submit({make(mid,2,0x00070007u)});
        batch=live.record(cmd,0);ss.observe(live,batch);lightDispatches.clear();
        assert(sky.recordStreamPreviewSky(cmd,0,live,handle,bits,preview,{},true,4));
        assert(lightDispatches.size()==1&&lightDispatches[0].jobCount==28);sky.retire();
        assert(select().jobs==28);
        // Current transparency table, not historical classification, decides cutoff.
        auto openBits=bits;openBits[0]|=1u<<7;
        lightDispatches.clear();assert(sky.recordStreamPreviewSky(cmd,0,live,handle,openBits,preview,{},true,4));
        assert(lightDispatches[0].jobCount==32);sky.retire();
        live.evict(roof);batch=live.record(cmd,0);ss.observe(live,batch);assert(ss.pending(center)&&select().keys.empty());
        watchResidentLight=false;live.shutdown();source.reset();
    }
    std::cout<<"Stage143/144/145/146 opaque roof cutoff, dense plane, preview budget, opaque ceiling, light-only, palette overflow, overlay rejection and reopening PASS (mock API)\n";
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
    // Actual Stage126 recorders, mocked API. No SPIR-V execution.
    {
        using R=VulkanColumnMaterialRuntime;using P=R::Plan;
        const auto dir=std::filesystem::temp_directory_path()/"opticraft-column-recorder-test";
        std::filesystem::create_directories(dir);
        const std::array<uint32_t,5> spv{0x07230203u,0,0,0,0};
        for(const char* name:{"worldgen_material_input.comp","worldgen_river_pack.comp","worldgen_cave_batch_column_pack.comp","worldgen_material.comp","worldgen_climate.comp","worldgen_terrain.comp","worldgen_layers.comp","worldgen_layer_merge.comp","worldgen_river_column_resolve.comp","worldgen_river_column_merge.comp","worldgen_cave_carve.comp","worldgen_cave_resolve.comp","worldgen_cave_column_merge.comp"}){
            std::ofstream f(dir/(std::string(name)+".spv"),std::ios::binary);f.write(reinterpret_cast<const char*>(spv.data()),sizeof(spv));
        }
        R runtime;runtime.initialize(physical,device,true,dir);
        std::array<int32_t,2> ys{0,-1};std::array<uint32_t,10> ids{1,2,3,4,5,6,7,8,9,10};
        const std::array<Gpu_Cave_Dispatch::BatchColumn,1> cave{{{0,0,63,0,0}}};
        R::Request req{0,0,63,-256,1024,4,5,false,ys,ids,32,P::Input::Region{0,0},cave};
        P plan;P::Limits limits{true,65535,65535,65535,1024,1024,16,16,128,128*1024*1024,256};
        assert(P::make(0,0,ys,63,4,5,false,-256,1024,ids,32,req.river,cave,limits,plan));
        std::array<VulkanDeviceBuffer,size_t(P::Resource::Count)> storage;
        R::Sources src{};
        for(size_t i=0;i<src.size();++i){auto r=P::Resource(i);if(r==P::Resource::Columns||r==P::Resource::InputWords||r==P::Resource::Output||!plan.bytes[i])continue;
            storage[i].create(physical,device,plan.bytes[i],VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            src[i]={storage[i].buffer(),0,plan.bytes[i],plan.bytes[i]};}
        watchColumn=true;columnDispatches.clear();
        auto invalid=src;invalid[0].bytes--;assert(!runtime.record(cmd,req,invalid));assert(columnDispatches.empty());
        assert(runtime.record(cmd,req,src));assert(runtime.busy()&&!runtime.record(cmd,req,src));
        assert(columnDispatches.size()==4);
        for(size_t i=0;i<4;++i){const auto& d=columnDispatches[i];const auto& step=plan.steps[i];assert(d.x==step.x&&d.y==step.y&&d.z==step.z&&d.pushBytes==step.pushBytes);assert(descriptorState.at(d.set)->bindings.size()==step.bindingCount);for(uint32_t j=0;j<step.bindingCount;++j){const auto& bind=step.bindings[j];assert(descriptorState.at(d.set)->bindings.at(bind.slot).range==plan.bytes[size_t(bind.resource)]);}}
        auto output=runtime.result();assert(output.buffer&&output.bytes==plan.certificateOffset);
        assert(runtime.certificates()==std::vector<uint32_t>({0,0})); // fake dispatch doesn't execute
        runtime.retire();assert(!runtime.result().buffer);
        req.levels=std::span(ys).first(1);req.river.reset();req.cave={};
        src[size_t(P::Resource::RiverProfiles)]={};src[size_t(P::Resource::CaveKinds)]={};
        columnDispatches.clear();assert(runtime.record(cmd,req,src)&&columnDispatches.size()==2);
        assert(runtime.result().buffer==output.buffer&&runtime.result().bytes==65536);
        runtime.retire();
        // Live prepared path borrows the old packed inputs, no init/pack dispatch.
        P prepared;assert(P::makePrepared(0,0,ys,-256,1024,ids,32,limits,prepared));
        src[size_t(P::Resource::Layers)]={};
        for(auto r:{P::Resource::Columns,P::Resource::InputWords}){
            auto i=size_t(r);storage[i].create(physical,device,prepared.bytes[i],VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
            src[i]={storage[i].buffer(),0,prepared.bytes[i],storage[i].size()};
        }
        columnDispatches.clear();auto wrong=src;wrong[size_t(P::Resource::InputWords)].bytes-=4;
        assert(!runtime.recordPrepared(cmd,0,0,ys,-256,1024,ids,32,wrong)&&columnDispatches.empty());
        assert(runtime.recordPrepared(cmd,0,0,ys,-256,1024,ids,32,src));
        assert(columnDispatches.size()==1&&columnDispatches[0].x==1024&&columnDispatches[0].pushBytes==64);
        const auto& bindings=descriptorState.at(columnDispatches[0].set)->bindings;
        assert(bindings.at(4).buffer==src[size_t(P::Resource::Columns)].buffer);
        assert(bindings.at(5).buffer==src[size_t(P::Resource::InputWords)].buffer);
        assert(runtime.result().buffer==output.buffer&&runtime.result().bytes==131072);
        assert(runtime.certificates()==std::vector<uint32_t>({0,0}));
        assert(!runtime.recordPrepared(cmd,0,0,ys,-256,1024,ids,32,src));
        runtime.retire();assert(!runtime.climateAccepted());
        Gpu_Climate_Tables climate{};runtime.initializeClimate(climate,dir);runtime.initializeRiver(dir);runtime.initializeCave(dir);std::filesystem::remove_all(dir);
        Gpu_Layer_Dispatch::Push layer{0,0,123,0,0};
        columnDispatches.clear();layer.origin_x=32;
        assert(!runtime.recordClimatePrepared(cmd,0,0,ys,-256,1024,ids,32,src,layer)&&columnDispatches.empty());
        layer.origin_x=0;
        assert(runtime.recordClimatePrepared(cmd,0,0,ys,-256,1024,ids,32,src,layer));
        assert(columnDispatches.size()==5);
        const std::array<uint32_t,5> dispatchX{18,16,16,16,1024},pushBytes{8,8,20,4,64},bindingCount{4,2,2,6,7};
        for(size_t i=0;i<5;++i){const auto& d=columnDispatches[i];assert(d.x==dispatchX[i]&&d.y==1&&d.z==1&&d.pushBytes==pushBytes[i]);assert(descriptorState.at(d.set)->bindings.size()==bindingCount[i]);}
        const auto& merge=descriptorState.at(columnDispatches[3].set)->bindings;
        const auto& base=descriptorState.at(columnDispatches[4].set)->bindings;
        assert(merge.at(1).buffer==src[size_t(P::Resource::Columns)].buffer&&merge.at(2).buffer==src[size_t(P::Resource::InputWords)].buffer);
        assert(merge.at(3).buffer==base.at(4).buffer&&merge.at(4).buffer==base.at(5).buffer);
        assert(merge.at(3).buffer!=merge.at(1).buffer&&merge.at(4).buffer!=merge.at(2).buffer);
        assert(runtime.climateAccepted()==0&&runtime.certificates()==std::vector<uint32_t>({0,0}));
        assert(!runtime.recordClimatePrepared(cmd,0,0,ys,-256,1024,ids,32,src,layer));
        runtime.retire();assert(!runtime.climateAccepted());
        using D=Gpu_River_Dispatch;
        static_assert(D::region_of_chunk(-1)==-1&&D::region_of_chunk(-8)==-1&&D::region_of_chunk(-9)==-2&&D::region_of_chunk(8)==1);
        auto packet=std::make_shared<D::CompactPacket>();packet->sea=63;
        packet->coarse.assign(D::compact_coarse_cells,64);packet->graph.assign(D::compact_cells,0);
        auto riverCall=[&](auto p){return runtime.recordClimateRiverPrepared(cmd,0,0,ys,-256,1024,ids,32,src,layer,p);};
        columnDispatches.clear();packet->region_x=-1;assert(!riverCall(packet)&&columnDispatches.empty());packet->region_x=0;
        packet->graph[0]=3;assert(!riverCall(packet)&&columnDispatches.empty());packet->graph[0]=0;
        assert(riverCall(packet)&&columnDispatches.size()==7);
        assert(columnDispatches[4].x==25&&columnDispatches[4].pushBytes==12&&columnDispatches[5].x==16&&columnDispatches[5].pushBytes==4);
        const auto& resolve=descriptorState.at(columnDispatches[4].set)->bindings;
        const auto& riverMerge=descriptorState.at(columnDispatches[5].set)->bindings;
        const auto& riverBase=descriptorState.at(columnDispatches[6].set)->bindings;
        assert(resolve.at(0).range==D::compact_coarse_cells*4&&resolve.at(1).range==D::compact_cells*4&&resolve.at(2).range==1600*16);
        assert(resolve.at(2).buffer==riverMerge.at(0).buffer&&riverMerge.at(1).buffer==src[size_t(P::Resource::InputWords)].buffer);
        assert(riverMerge.at(2).buffer==riverBase.at(5).buffer&&riverMerge.at(2).buffer!=riverMerge.at(1).buffer);
        const auto graphBuffer=resolve.at(1).buffer;
        assert(runtime.riverAccepted()==0&&runtime.climateAccepted()==0&&!riverCall(packet));
        runtime.retire();assert(!runtime.riverAccepted());columnDispatches.clear();
        assert(riverCall(packet)&&descriptorState.at(columnDispatches[4].set)->bindings.at(1).buffer==graphBuffer);
        runtime.retire();columnDispatches.clear();assert(runtime.recordClimatePrepared(cmd,0,0,ys,-256,1024,ids,32,src,layer)&&!runtime.riverAccepted());
        runtime.retire();columnDispatches.clear();
        Gpu_Cave_Dispatch::Packet cp;assert(Gpu_Cave_Dispatch::make(0,0,63,cp.push));
        cp.stamps.push_back({1,0,1,3,2,0,0});cp.push.count=1;
        assert(runtime.recordClimateRiverPrepared(cmd,0,0,ys,-256,1024,ids,32,src,layer,packet,&cp));
        assert(columnDispatches.size()==10&&runtime.caveAccepted()==0);
        const auto& carve=descriptorState.at(columnDispatches[6].set)->bindings;
        const auto& caveResolve=descriptorState.at(columnDispatches[7].set)->bindings;
        const auto& caveMerge=descriptorState.at(columnDispatches[8].set)->bindings;
        assert(columnDispatches[6].x==1&&columnDispatches[7].x==562&&columnDispatches[8].x==32&&columnDispatches[8].y==2);
        assert(carve.at(5).buffer==caveResolve.at(5).buffer&&caveResolve.at(6).buffer==caveMerge.at(0).buffer);
        assert(caveMerge.at(2).buffer==descriptorState.at(columnDispatches[9].set)->bindings.at(5).buffer);
        runtime.retire();columnDispatches.clear();assert(!runtime.caveAccepted());
        cp.push.count=0;cp.stamps.clear();
        assert(runtime.recordClimatePrepared(cmd,0,0,ys,-256,1024,ids,32,src,layer,&cp));
        assert(columnDispatches.size()==7&&runtime.caveAccepted()==0&&!runtime.riverAccepted());
        runtime.retire();columnDispatches.clear();cp.push.origin_x=32;
        assert(runtime.recordClimatePrepared(cmd,0,0,ys,-256,1024,ids,32,src,layer,&cp));
        assert(columnDispatches.size()==5&&!runtime.caveAccepted());
        runtime.retire();watchColumn=false;
    }
    std::cout<<"Stage126 column Vulkan allocation, sparse descriptors, dispatch order, busy/reuse/certificates PASS (mock API)\n";
    std::cout<<"Stage130 climate Vulkan allocation, five passes, disjoint input/output, stats and reuse PASS (mock API)\n";
    std::cout<<"Stage131 local river Vulkan allocation, seven passes, guarded source, reuse and stats PASS (mock API)\n";
    {
        const std::array<uint32_t,1> spv{0x07230203u};auto renderPass=reinterpret_cast<VkRenderPass>(4);
        auto atlasLayout=reinterpret_cast<VkDescriptorSetLayout>(5);
        VulkanEntityPipeline entities;entities.initialize(physical,device,renderPass,atlasLayout,spv,spv);
        VulkanItemPipeline items;items.initialize(physical,device,renderPass,atlasLayout,spv,spv);
        VulkanFrameRuntime::Frame frame{};frame.cmd=cmd;frame.extent={800,600};frame.frameSlot=0;
        WorldViewParams view{};view.vp={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        auto frustum=GpuFrustum::fromVulkanViewProjection(view.vp.data());
        auto a=std::make_shared<EntityAsset>();a->vertices.resize(3);a->vertices[1].position[0]=.5f;a->vertices[2].position[1]=.5f;
        a->indices={0,1,2,0,1,2};a->textures.resize(1);a->textures[0].width=1;a->textures[0].height=1;a->textures[0].rgba={255,255,255,255};a->primitives={{0,3,0,"body"},{3,3,0,"hand"}};
        std::vector<EntityDraw> draws(1002);for(auto& d:draws){d.asset=a;d.pose.model=view.vp;}
        draws[1000].hiddenMaterial="hand";draws[1001].pose.model[12]=5.f;
        // Entity texture upload uses the original atlas path. Reset counters
        // after this new test so the historical atlas checks stay independent.
        const auto oldCopies=atlasCopyCalls,oldBlits=atlasBlitRegions;
        entities.prepare(frame,draws,frustum);watchGraphics=true;graphicsDraws.clear();entities.draw(frame,view);
        assert(graphicsDraws.size()==3&&graphicsDraws[0].instances==1000&&graphicsDraws[1].instances==1000&&graphicsDraws[2].instances==1&&graphicsDraws[2].firstInstance==1000);
        assert(entities.stats().submitted==1002&&entities.stats().visible==1001&&entities.stats().culled==1&&entities.stats().drawCalls==3&&entities.stats().unbatchedDrawCalls==2001);
        frame.frameSlot=1;entities.prepare(frame,{},frustum);graphicsDraws.clear();entities.draw(frame,view);assert(graphicsDraws.empty());
        ItemVertex v{},w{},x{},y{};w.x=1;x.x=x.y=1;y.y=1;
        std::array<ItemVertex,6> quad{v,w,x,v,x,y};items.prepare(frame,quad);items.draw(frame,reinterpret_cast<VkDescriptorSet>(6),view);
        assert(graphicsDraws.size()==1&&graphicsDraws[0].indexed&&graphicsDraws[0].count==6);
        assert(items.stats().sourceBytes==216&&items.stats().uploadBytes==168&&items.stats().indexed);
        frame.frameSlot=0;graphicsDraws.clear();items.prepare(frame,std::span(quad).first(3));items.draw(frame,reinterpret_cast<VkDescriptorSet>(6),view);
        assert(graphicsDraws.size()==1&&!graphicsDraws[0].indexed&&graphicsDraws[0].count==3);
        items.prepare(frame,{});graphicsDraws.clear();items.draw(frame,reinterpret_cast<VkDescriptorSet>(6),view);assert(graphicsDraws.empty());
        watchGraphics=false;atlasCopyCalls=oldCopies;atlasBlitRegions=oldBlits;
    }
    std::cout<<"Stage126 actual entity instancing + frustum, indexed item and empty-frame recording PASS (mock API)\n";

}
