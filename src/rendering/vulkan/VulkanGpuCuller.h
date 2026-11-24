#pragma once
#include "rendering/vulkan/GpuFrustum.h"
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <span>

namespace OptiCraft::Vulkan {
// Written indirect array must be independent of the mesher's published commands
// and unique to the in-flight frame slot; otherwise compute overwrites a draw
// while earlier frames still execute it. Renderer owns memory and frame fences.
struct VulkanCullerBuffers {
    std::array<VkBuffer, 4> buffers{};
    std::array<VkDeviceSize, 4> byteSizes{};
    uint32_t chunkSlotStride = 64; // 64B opaque slots, 96B misc slots
    // 0 stable chunk slots (64B), 1 published indirect draws (16B),
    // 2 per-frame live slot IDs (4B), 3 per-frame culled draw commands (16B).
};
class VulkanGpuCuller {
public:
    VulkanGpuCuller() = default;
    ~VulkanGpuCuller();
    VulkanGpuCuller(const VulkanGpuCuller&) = delete;
    VulkanGpuCuller& operator=(const VulkanGpuCuller&) = delete;
    void initialize(VkPhysicalDevice physical, VkDevice device,
                    const VulkanCullerBuffers& buffers,
                    std::span<const uint32_t> spirv);
    // No calls/dispatch if count=0. Frame buffer and slots must be uploaded
    // prior to this call and correct transfer->compute barriers inserted.
    void record(VkCommandBuffer cmd, uint32_t count, const GpuFrustum& planes) const;
    void shutdown(); // must not destroy before all referencing GPU work finishes
    uint32_t maxJobs() const noexcept { return maxJobs_; }
    VkBuffer culledDrawBuffer() const noexcept { return buffers_.buffers[3]; }
private:
    struct alignas(16) Params {
        GpuFrustum frustum;
        uint32_t firstJob = 0, totalJobs = 0, pad0 = 0, pad1 = 0;
    };
    static_assert(sizeof(Params) == 112);
    VkDevice device_ = VK_NULL_HANDLE;
    VulkanCullerBuffers buffers_{};
    VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    uint32_t maxJobs_ = 0, maxGroupsX_ = 0;
};
} // namespace OptiCraft::Vulkan
