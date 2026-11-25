#pragma once

#include "rendering/vulkan/VulkanDeviceBuffer.h"
#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>
#include <vector>

namespace OptiCraft::Vulkan {

// The real blocks_main PNG, uploaded once. All 16x16 sprites use a GPU-built
// tile-isolated mip chain, never a whole-atlas blit which contaminates nearby
// sprites. No CPU resampling or OpenGL texture object is involved.
class VulkanBlockAtlas {
public:
    VulkanBlockAtlas() = default;
    ~VulkanBlockAtlas() { shutdown(); }
    VulkanBlockAtlas(const VulkanBlockAtlas&) = delete;
    VulkanBlockAtlas& operator=(const VulkanBlockAtlas&) = delete;
    void initialize(VkPhysicalDevice physical,VkDevice device,
                    const std::string& pngPath,uint32_t tileSize=16);
    void initializePixels(VkPhysicalDevice,VkDevice,uint32_t width,uint32_t height,const uint8_t* rgba,uint32_t tileSize=1,bool repeat=false);
    // Record once before first fragment sampling. Staging must remain alive
    // until the submission fence has signalled.
    void recordUpload(VkCommandBuffer primary);
    void releaseStagingAfterCompletion();
    // descriptor layout comes from VulkanMiscPipeline::atlasSetLayout().
    VkDescriptorSet createDescriptorSet(VkDescriptorSetLayout layout);
    void shutdown() noexcept; // GPU must no longer reference the image/set
    VkImageView view() const noexcept {return view_;}
    VkSampler sampler() const noexcept {return sampler_;}
    uint32_t mipLevels() const noexcept {return mipLevels_;}
    uint32_t width() const noexcept {return width_;}
    uint32_t height() const noexcept {return height_;}
private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VulkanDeviceBuffer staging_;
    uint32_t width_=0,height_=0,tileSize_=0,mipLevels_=0;
    bool recorded_=false;
};

} // namespace OptiCraft::Vulkan
