#pragma once
#include "rendering/vulkan/VulkanFrameRuntime.h"
#include "rendering/vulkan/VulkanDeviceBuffer.h"
#include "rendering/vulkan/WorldViewParams.h"
#include <array>
#include <span>
namespace OptiCraft::Vulkan {
struct ItemVertex {float x,y,z,u,v,nx,ny,nz,light;};
static_assert(sizeof(ItemVertex)==36);
// Dynamic item geometry uses the original gameplay mesh and block atlas.
class VulkanItemPipeline {
public:
    ~VulkanItemPipeline(){shutdown();}
    void initialize(VulkanFrameRuntime&,VkDescriptorSetLayout atlasLayout,std::span<const uint32_t> vs,std::span<const uint32_t> fs);
    void recreate(VkRenderPass,std::span<const uint32_t> vs,std::span<const uint32_t> fs);
    void prepare(const VulkanFrameRuntime::Frame&,std::span<const ItemVertex>);
    void draw(const VulkanFrameRuntime::Frame&,std::span<const ItemVertex>,VkDescriptorSet,const WorldViewParams&)const;
    void shutdown()noexcept;
private:
    VkDevice device_=VK_NULL_HANDLE;
    VkPhysicalDevice physical_=VK_NULL_HANDLE;
    VkPipelineLayout layout_=VK_NULL_HANDLE;
    VkPipeline pipeline_=VK_NULL_HANDLE;
    std::array<VulkanDeviceBuffer,VulkanFrameRuntime::kFramesInFlight> buffers_{};
};
}
