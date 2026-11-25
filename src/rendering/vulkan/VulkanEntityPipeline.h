#pragma once
#include "rendering/vulkan/EntityBatchPlan.h"
#include "rendering/vulkan/VulkanFrameRuntime.h"
#include "rendering/vulkan/VulkanDeviceBuffer.h"
#include "rendering/vulkan/VulkanBlockAtlas.h"
#include "rendering/vulkan/WorldViewParams.h"
#include <span>
#include <unordered_map>
namespace OptiCraft::Vulkan {
class VulkanEntityPipeline {
public:
    ~VulkanEntityPipeline(){shutdown();}
    void initialize(VulkanFrameRuntime&,VkDescriptorSetLayout atlasLayout,std::span<const uint32_t>,std::span<const uint32_t>);
    void initialize(VkPhysicalDevice,VkDevice,VkRenderPass,VkDescriptorSetLayout,std::span<const uint32_t>,std::span<const uint32_t>);
    void recreate(VkRenderPass,std::span<const uint32_t>,std::span<const uint32_t>);
    void prepare(const VulkanFrameRuntime::Frame&,std::span<const EntityDraw>,const GpuFrustum&);
    void draw(const VulkanFrameRuntime::Frame&,const WorldViewParams&)const;
    void shutdown()noexcept;
    struct Stats {size_t submitted=0,visible=0,culled=0,drawCalls=0,unbatchedDrawCalls=0;};
    const Stats& stats()const noexcept{return stats_;}
private:
    struct AssetGpu {
        std::shared_ptr<const EntityAsset> owner;
        VulkanDeviceBuffer vertices,indices;
        std::vector<std::unique_ptr<VulkanBlockAtlas>> textures;
        std::vector<VkDescriptorSet> sets;
        uint32_t uploadSlot=UINT32_MAX;
    };
    struct PoseFrame {VulkanDeviceBuffer buffer;VkDescriptorSet set=VK_NULL_HANDLE;EntityBatchPlan batch;};
    Stats stats_{};
    VkDevice device_=VK_NULL_HANDLE;
    VkPhysicalDevice physical_=VK_NULL_HANDLE;
    VkDescriptorSetLayout poseLayout_=VK_NULL_HANDLE,atlasLayout_=VK_NULL_HANDLE;
    VkDescriptorPool pool_=VK_NULL_HANDLE;
    VkPipelineLayout layout_=VK_NULL_HANDLE;
    VkPipeline pipeline_=VK_NULL_HANDLE;
    VkDeviceSize maxPoseBytes_=0;
    std::array<PoseFrame,VulkanFrameRuntime::kFramesInFlight> frames_{};
    std::unordered_map<const EntityAsset*,AssetGpu> assets_;
    std::unordered_map<const EntityAsset*,std::pair<std::shared_ptr<const EntityAsset>,EntityWorldBounds>> bounds_;
};
}
