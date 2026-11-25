#pragma once
#include "rendering/vulkan/EntityDraw.h"
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
    void recreate(VkRenderPass,std::span<const uint32_t>,std::span<const uint32_t>);
    void prepare(const VulkanFrameRuntime::Frame&,std::span<const EntityDraw>);
    void draw(const VulkanFrameRuntime::Frame&,std::span<const EntityDraw>,const WorldViewParams&)const;
    void shutdown()noexcept;
private:
    struct AssetGpu {
        std::shared_ptr<const EntityAsset> owner;
        VulkanDeviceBuffer vertices,indices;
        std::vector<std::unique_ptr<VulkanBlockAtlas>> textures;
        std::vector<VkDescriptorSet> sets;
        uint32_t uploadSlot=UINT32_MAX;
    };
    struct PoseFrame {VulkanDeviceBuffer buffer;VkDescriptorSet set=VK_NULL_HANDLE;};
    VkDevice device_=VK_NULL_HANDLE;
    VkPhysicalDevice physical_=VK_NULL_HANDLE;
    VkDescriptorSetLayout poseLayout_=VK_NULL_HANDLE,atlasLayout_=VK_NULL_HANDLE;
    VkDescriptorPool pool_=VK_NULL_HANDLE;
    VkPipelineLayout layout_=VK_NULL_HANDLE;
    VkPipeline pipeline_=VK_NULL_HANDLE;
    VkDeviceSize maxPoseBytes_=0;
    std::array<PoseFrame,VulkanFrameRuntime::kFramesInFlight> frames_{};
    std::unordered_map<const EntityAsset*,AssetGpu> assets_;
    std::vector<EntityPose> poses_;
};
}
