#pragma once
#include "rendering/vulkan/VulkanFrameRuntime.h"
#include "rendering/vulkan/VulkanDeviceBuffer.h"
#include "rendering/vulkan/WorldViewParams.h"
#include "rendering/vulkan/ItemQuadIndexPlan.h"
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
    void initialize(VkPhysicalDevice,VkDevice,VkRenderPass,VkDescriptorSetLayout,std::span<const uint32_t>,std::span<const uint32_t>);
    void recreate(VkRenderPass,std::span<const uint32_t> vs,std::span<const uint32_t> fs);
    void prepare(const VulkanFrameRuntime::Frame&,std::span<const ItemVertex>);
    void draw(const VulkanFrameRuntime::Frame&,VkDescriptorSet,const WorldViewParams&)const;
    void shutdown()noexcept;
    struct Stats {uint64_t sourceBytes=0,uploadBytes=0;bool indexed=false;};
    const Stats& stats()const noexcept{return stats_;}
private:
    Stats stats_{};
    VkDevice device_=VK_NULL_HANDLE;
    VkPhysicalDevice physical_=VK_NULL_HANDLE;
    VkPipelineLayout layout_=VK_NULL_HANDLE;
    VkPipeline pipeline_=VK_NULL_HANDLE;
    struct ItemFrame {VulkanDeviceBuffer vertices,indices;uint32_t count=0;bool indexed=false;};
    std::array<ItemFrame,VulkanFrameRuntime::kFramesInFlight> buffers_{};
    ItemQuadIndexPlan<ItemVertex> scratch_;
};
}
