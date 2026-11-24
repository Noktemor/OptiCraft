#pragma once
#include "rendering/vulkan/VulkanUiBatch.h"
#include "rendering/vulkan/VulkanBlockAtlas.h"
#include "rendering/vulkan/VulkanFrameRuntime.h"
#include <array>
#include <span>
namespace OptiCraft::Vulkan {
class VulkanUiPipeline {
public:
    ~VulkanUiPipeline(){shutdown();}
    void initialize(VulkanFrameRuntime&,std::span<const uint32_t> vs,std::span<const uint32_t> fs);
    void recreate(VkRenderPass,std::span<const uint32_t> vs,std::span<const uint32_t> fs);
    void prepare(const VulkanFrameRuntime::Frame&,const UiBatch&);
    void draw(const VulkanFrameRuntime::Frame&,const UiBatch&,VkDescriptorSet blocks)const;
    void shutdown()noexcept;
private:
    VkDevice device_=VK_NULL_HANDLE;
    VkPhysicalDevice physical_=VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_=VK_NULL_HANDLE;
    VkDescriptorSet uiSet_=VK_NULL_HANDLE;
    VkPipelineLayout layout_=VK_NULL_HANDLE;
    VkPipeline pipeline_=VK_NULL_HANDLE;
    VulkanBlockAtlas atlas_;
    std::array<VulkanDeviceBuffer,VulkanFrameRuntime::kFramesInFlight> buffers_{};
    uint32_t uploadSlot_=UINT32_MAX;
    bool released_=false;
};
}
