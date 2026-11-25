#pragma once
#include "rendering/vulkan/VulkanDeviceBuffer.h"
#include "world/generation/Gpu_Worldgen_Data.h"
#include <array>
#include <vector>
#include <chrono>
class World_Generator;
namespace OptiCraft::Vulkan {
class VulkanFrameRuntime;
// Main-thread only; borrowed device/queue must outlive this object.
class VulkanMaterialWorldgen final {
public:
    explicit VulkanMaterialWorldgen(VulkanFrameRuntime& runtime):runtime_(runtime){}
    ~VulkanMaterialWorldgen();
    VulkanMaterialWorldgen(const VulkanMaterialWorldgen&)=delete;
    VulkanMaterialWorldgen& operator=(const VulkanMaterialWorldgen&)=delete;
    bool start(const World_Generator&,int,int,const std::array<Gpu_Material_Column,1024>&,const std::vector<int>&);
    bool poll(std::vector<uint32_t>&,double&);
    void abandon() noexcept; // invalidate result; retain in-flight resources until destruction
    bool enabled()const noexcept{return !disabled_;}
private:
    void initialize(const World_Generator&);
    VulkanFrameRuntime& runtime_;
    std::array<VulkanDeviceBuffer,7> buffers_;
    VkDescriptorSetLayout layout_=VK_NULL_HANDLE;
    VkDescriptorPool descriptors_=VK_NULL_HANDLE;
    VkDescriptorSet set_=VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_=VK_NULL_HANDLE;
    VkPipeline pipeline_=VK_NULL_HANDLE;
    VkCommandPool pool_=VK_NULL_HANDLE;
    VkCommandBuffer cmd_=VK_NULL_HANDLE;
    VkFence fence_=VK_NULL_HANDLE;
    VkQueryPool timestamps_=VK_NULL_HANDLE;
    uint32_t timestampBits_=0;
    double timestampNs_=0;
    size_t count_=0;
    bool submitted_=false, disabled_=false;
    std::chrono::steady_clock::time_point started_{};
};
}
