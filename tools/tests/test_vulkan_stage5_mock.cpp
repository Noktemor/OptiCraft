// Host-only Vulkan API simulation. It validates descriptor source ABI,
// buffer offsets, batch copying and slot lifecycle. It is NOT GPU validation.
#include "rendering/vulkan/VulkanChunkStreamGpu.h"
#include "rendering/vulkan/VulkanGpuMesher.h"
#include "rendering/vulkan/VulkanMiscMesher.h"
#include "rendering/vulkan/VulkanMaterialTables.h"
#include "rendering/vulkan/VulkanBlockAtlas.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>
using namespace OptiCraft::Vulkan;
namespace {
struct FakeMemory {std::vector<std::byte> bytes;};
struct FakeBuffer {VkDeviceSize size;FakeMemory* memory=nullptr;};
struct FakeImage {uint32_t width=0,height=0,mips=0;FakeMemory* memory=nullptr;};
size_t atlasBlitRegions=0,atlasCopyCalls=0;
size_t copies=0,fills=0,liveCopies=0;
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
    *result=new FakeBuffer{info->size};return VK_SUCCESS;
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
}
VkResult vkAllocateMemory(VkDevice,const VkMemoryAllocateInfo* info,const void*,VkDeviceMemory* mem){
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
void vkDestroyBuffer(VkDevice,VkBuffer buf,const void*){delete static_cast<FakeBuffer*>(buf);}
void vkFreeMemory(VkDevice,VkDeviceMemory mem,const void*){delete static_cast<FakeMemory*>(mem);}
void vkCmdPipelineBarrier(VkCommandBuffer,VkPipelineStageFlags,VkPipelineStageFlags,
    uint32_t,uint32_t,const VkMemoryBarrier*,uint32_t,const void*,uint32_t,const void*){}
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
    assert(info->maxLod==4.0f && info->minFilter==VK_FILTER_NEAREST);
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
    assert(info->maxSets==1);*pool=new int(3);return VK_SUCCESS;
}
VkResult vkAllocateDescriptorSets(VkDevice,const VkDescriptorSetAllocateInfo* info,
                                  VkDescriptorSet* set){
    assert(info->descriptorSetCount==1);*set=reinterpret_cast<void*>(0x55);return VK_SUCCESS;
}
void vkUpdateDescriptorSets(VkDevice,uint32_t count,const VkWriteDescriptorSet* writes,
                            uint32_t,const void*){
    assert(count==1 && writes[0].pImageInfo->imageLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}
void vkDestroyDescriptorPool(VkDevice,VkDescriptorPool pool,const void*){delete static_cast<int*>(pool);}
}
int main(int argc,char** argv){
    auto device=reinterpret_cast<VkDevice>(1);
    auto physical=reinterpret_cast<VkPhysicalDevice>(2);
    auto cmd=reinterpret_cast<VkCommandBuffer>(3);
    VulkanChunkStreamGpu scene;
    scene.initialize(physical,device,{4,2,8});
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
    std::cout<<"Vulkan Stage 5 mocked transfer/slot/material/atlas tests passed\n";
}
