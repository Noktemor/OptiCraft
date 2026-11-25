#include "rendering/vulkan/VulkanBlockAtlas.h"

// Static stb symbols avoid ODR conflicts with the OpenGL Texture.cpp loader.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace OptiCraft::Vulkan {
namespace {
void check(VkResult result,const char* name){
    if(result!=VK_SUCCESS)throw std::runtime_error(std::string(name)+": "+std::to_string(result));
}
uint32_t localMemory(VkPhysicalDevice physical,uint32_t allowed){
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical,&props);
    for(uint32_t i=0;i<props.memoryTypeCount;i++)
        if((allowed&(1u<<i)) &&
            (props.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))return i;
    throw std::runtime_error("Block atlas requires device-local image memory");
}
void transition(VkCommandBuffer cmd,VkImage image,uint32_t base,uint32_t count,
                VkImageLayout before,VkImageLayout after,
                VkPipelineStageFlags sourceStage,VkPipelineStageFlags targetStage,
                VkAccessFlags srcAccess,VkAccessFlags dstAccess){
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask=srcAccess;b.dstAccessMask=dstAccess;
    b.oldLayout=before;b.newLayout=after;
    b.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    b.image=image;
    b.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel=base;
    b.subresourceRange.levelCount=count;
    b.subresourceRange.layerCount=1;
    vkCmdPipelineBarrier(cmd,sourceStage,targetStage,0,0,nullptr,0,nullptr,1,&b);
}
}
void VulkanBlockAtlas::initialize(VkPhysicalDevice physical,VkDevice device,
                                   const std::string& pngPath,uint32_t tileSize){
    if(device_ || !physical || !device || !tileSize ||
       (tileSize&(tileSize-1)) || tileSize>1024)
        throw std::invalid_argument("Invalid tile size or Vulkan device for blocks atlas");
    int w=0,h=0,n=0;
    stbi_set_flip_vertically_on_load(0); // static stb instance; shaders convert atlas V
    stbi_uc* image=stbi_load(pngPath.c_str(),&w,&h,&n,STBI_rgb_alpha);
    if(!image)throw std::runtime_error("Cannot decode blocks atlas PNG: "+pngPath);
    try { if(w<=0||h<=0)throw std::runtime_error("Invalid image size");initializePixels(physical,device,uint32_t(w),uint32_t(h),image,tileSize); }
    catch(...){stbi_image_free(image);throw;}
    stbi_image_free(image);
}
void VulkanBlockAtlas::initializePixels(VkPhysicalDevice physical,VkDevice device,uint32_t w,uint32_t h,const uint8_t* image,uint32_t tileSize,bool repeat){
    if(device_||!physical||!device||!image||!tileSize||(tileSize&(tileSize-1))||tileSize>1024)
        throw std::invalid_argument("Invalid raw atlas input");
    try {
        if(!w||!h||w%tileSize||h%tileSize)
            throw std::invalid_argument("Block atlas dimensions must align to 16px sprites");
        width_=uint32_t(w);height_=uint32_t(h);tileSize_=tileSize;
        mipLevels_=1;
        for(uint32_t side=tileSize;side>1;side>>=1)++mipLevels_;
        const VkDeviceSize bytes=VkDeviceSize(width_)*height_*4;
        physical_=physical;device_=device;
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physical_,VK_FORMAT_R8G8B8A8_SRGB,&properties);
        const auto required=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | (mipLevels_>1 ?
            (VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT|
             VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) : 0u);
        if((properties.optimalTilingFeatures&required)!=required)
            throw std::runtime_error("GPU cannot generate tile-isolated linear-filtered SRGB atlas mips");
        VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        create.imageType=VK_IMAGE_TYPE_2D;
        create.format=VK_FORMAT_R8G8B8A8_SRGB;
        create.extent={width_,height_,1};
        create.mipLevels=mipLevels_;
        create.arrayLayers=1;
        create.samples=VK_SAMPLE_COUNT_1_BIT;
        create.tiling=VK_IMAGE_TILING_OPTIMAL;
        create.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|
                     VK_IMAGE_USAGE_SAMPLED_BIT;
        create.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        create.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        check(vkCreateImage(device_,&create,nullptr,&image_),"vkCreateImage(block atlas)");
        VkMemoryRequirements need{};
        vkGetImageMemoryRequirements(device_,image_,&need);
        VkMemoryAllocateInfo mem{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mem.allocationSize=need.size;
        mem.memoryTypeIndex=localMemory(physical_,need.memoryTypeBits);
        check(vkAllocateMemory(device_,&mem,nullptr,&memory_),"vkAllocateMemory(atlas)");
        check(vkBindImageMemory(device_,image_,memory_,0),"vkBindImageMemory(atlas)");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image=image_;view.viewType=VK_IMAGE_VIEW_TYPE_2D;
        view.format=VK_FORMAT_R8G8B8A8_SRGB;
        view.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount=mipLevels_;view.subresourceRange.layerCount=1;
        check(vkCreateImageView(device_,&view,nullptr,&view_),"vkCreateImageView(atlas)");
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter=VK_FILTER_NEAREST; // preserve pixel-art style
        sampler.minFilter=VK_FILTER_NEAREST;
        sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU=repeat?VK_SAMPLER_ADDRESS_MODE_REPEAT:VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeV=sampler.addressModeU;
        sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.maxLod=float(mipLevels_-1);
        sampler.borderColor=VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        check(vkCreateSampler(device_,&sampler,nullptr,&sampler_),"vkCreateSampler(atlas)");
        staging_.create(physical_,device_,bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        staging_.write(0,image,bytes);
    }catch(...){shutdown();throw;}
}
void VulkanBlockAtlas::recordUpload(VkCommandBuffer cmd){
    if(!device_||!cmd||!staging_||recorded_)
        throw std::logic_error("VulkanBlockAtlas upload already recorded or uninitialized");
    transition(cmd,image_,0,mipLevels_,VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT,0,VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount=1;
    copy.imageExtent={width_,height_,1};
    vkCmdCopyBufferToImage(cmd,staging_.buffer(),image_,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    // Blit each 16x16 tile independently. One vkCmdBlitImage call per mip level;
    // full-atlas blits would mix adjacent materials at mip boundaries.
    const uint32_t tilesX=width_/tileSize_,tilesY=height_/tileSize_;
    std::vector<VkImageBlit> tiles;
    if(mipLevels_>1)tiles.reserve(size_t(tilesX)*tilesY);
    for(uint32_t level=1;level<mipLevels_;++level){
        transition(cmd,image_,level-1,1,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_ACCESS_TRANSFER_READ_BIT);
        const int srcSize=int(tileSize_>>(level-1));
        const int dstSize=int(tileSize_>>level);
        tiles.clear();
        for(uint32_t y=0;y<tilesY;y++)for(uint32_t x=0;x<tilesX;x++){
            VkImageBlit blit{};
            blit.srcSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.mipLevel=level-1;
            blit.srcSubresource.layerCount=1;
            blit.srcOffsets[0]={int(x)*srcSize,int(y)*srcSize,0};
            blit.srcOffsets[1]={int(x+1)*srcSize,int(y+1)*srcSize,1};
            blit.dstSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
            blit.dstSubresource.mipLevel=level;
            blit.dstSubresource.layerCount=1;
            blit.dstOffsets[0]={int(x)*dstSize,int(y)*dstSize,0};
            blit.dstOffsets[1]={int(x+1)*dstSize,int(y+1)*dstSize,1};
            tiles.push_back(blit);
        }
        vkCmdBlitImage(cmd,image_,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       image_,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       uint32_t(tiles.size()),tiles.data(),VK_FILTER_LINEAR);
    }
    if(mipLevels_>1)
        transition(cmd,image_,0,mipLevels_-1,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_TRANSFER_READ_BIT,
                   VK_ACCESS_SHADER_READ_BIT);
    transition(cmd,image_,mipLevels_-1,1,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_ACCESS_SHADER_READ_BIT);
    recorded_=true;
}
VkDescriptorSet VulkanBlockAtlas::createDescriptorSet(VkDescriptorSetLayout layout){
    if(!device_||!recorded_||!layout||pool_)
        throw std::logic_error("VulkanBlockAtlas descriptor creation invalid state");
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets=1;poolInfo.poolSizeCount=1;poolInfo.pPoolSizes=&size;
    check(vkCreateDescriptorPool(device_,&poolInfo,nullptr,&pool_),"vkCreateDescriptorPool(atlas)");
    VkDescriptorSet set=VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool=pool_;alloc.descriptorSetCount=1;alloc.pSetLayouts=&layout;
    check(vkAllocateDescriptorSets(device_,&alloc,&set),"vkAllocateDescriptorSets(atlas)");
    VkDescriptorImageInfo info{};
    info.sampler=sampler_;info.imageView=view_;
    info.imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet=set;write.dstBinding=0;
    write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo=&info;
    vkUpdateDescriptorSets(device_,1,&write,0,nullptr);
    return set;
}
void VulkanBlockAtlas::releaseStagingAfterCompletion(){
    if(!recorded_)throw std::logic_error("Vulkan atlas upload not submitted");
    staging_.destroy();
}
void VulkanBlockAtlas::shutdown() noexcept {
    staging_.destroy();
    if(device_){
        if(pool_)vkDestroyDescriptorPool(device_,pool_,nullptr);
        if(sampler_)vkDestroySampler(device_,sampler_,nullptr);
        if(view_)vkDestroyImageView(device_,view_,nullptr);
        if(image_)vkDestroyImage(device_,image_,nullptr);
        if(memory_)vkFreeMemory(device_,memory_,nullptr);
    }
    pool_=VK_NULL_HANDLE;sampler_=VK_NULL_HANDLE;view_=VK_NULL_HANDLE;
    image_=VK_NULL_HANDLE;memory_=VK_NULL_HANDLE;device_=VK_NULL_HANDLE;
    physical_=VK_NULL_HANDLE;width_=height_=tileSize_=mipLevels_=0;recorded_=false;
}
} // namespace OptiCraft::Vulkan
