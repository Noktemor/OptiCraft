#pragma once

#include "rendering/vulkan/GpuWorldCore.h"
#include <vulkan/vulkan.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace OptiCraft::Vulkan {

// GPU-only quad meshing input. All offsets refer to persistent GPU-side arenas.
// Block word offset is 32-bit words; face offset is uvec2 entries (8 bytes).
// The neighbor offsets are -1 when that neighbor is absent/not resident.
struct alignas(16) GpuChunkSlot {
    std::array<uint32_t, 4> source{}; // blockBase, faceBase, faceCapacity, flags
    std::array<int32_t, 4> origin{};   // world-space origin
    std::array<int32_t, 4> neighborA{{-1, -1, -1, -1}};
    std::array<int32_t, 4> neighborB{{-1, -1, -1, -1}};
};
static_assert(sizeof(GpuChunkSlot) == 64 && alignof(GpuChunkSlot) == 16);

// Device buffers must be allocated/bound by the renderer, and stay alive during
// dispatch. Transfer/upload ownership, Vulkan queue synchronization, budget
// control, readback and old-mesh COW publication live in the caller.
struct GpuMesherBuffers {
    std::array<VkBuffer, 10> buffers{};
    std::array<VkDeviceSize, 10> byteSizes{};
    // bindings: 0 block words, 1 stable chunk slots, 2 per-batch counters,
    // 3 packed faces, 4 65536 block flags, 5 persistent indirect draws,
    // 6 per-batch stable slot indices, 7 candidate indirect draws,
    // 8 packed byte metadata and 9 packed light bytes for exact run compatibility.
};

class VulkanGpuMesher {
public:
    VulkanGpuMesher() = default;
    ~VulkanGpuMesher();
    VulkanGpuMesher(const VulkanGpuMesher&) = delete;
    VulkanGpuMesher& operator=(const VulkanGpuMesher&) = delete;

    // SPIR-V words are compiled from assets/vulkan_shaders/voxel_*.comp.
    // No per-frame vkCreate*, allocation or vkGetPhysicalDeviceProperties.
    void initialize(VkPhysicalDevice physical, VkDevice device, const GpuMesherBuffers& buffers,
                    std::span<const uint32_t> countSpv,
                    std::span<const uint32_t> emitSpv,
                    std::span<const uint32_t> finalizeSpv,
                    std::span<const uint32_t> publishSpv);
    void shutdown(); // must be called AFTER GPU work referencing these pipelines completes

    // Caller has uploaded chunk-slot table, neighbor data, dispatch indices and
    // material flags; Vulkan buffer barriers/ownership for that upload are required.
    // Count and Emit are intentionally separate submissions/frames: asynchronous
    // count readback determines exact arena capacity (no guess allocations).
    void recordCount(VkCommandBuffer cmd, uint32_t jobCount) const;
    void recordEmit(VkCommandBuffer cmd, uint32_t jobCount) const;
    // Only after emit completes and ALL candidates in this batch are validated.
    // No published draw is touched by count/emit/finalize.
    void recordPublish(VkCommandBuffer cmd, uint32_t jobCount) const;
    VkDescriptorSetLayout descriptorSetLayout() const { return descriptorLayout_; }
    VkDescriptorSet descriptorSet() const { return descriptorSet_; }
    uint32_t maxJobs() const { return maxJobs_; }
private:
    struct Params { uint32_t firstJob, totalJobs; };
    void dispatchMeshes(VkCommandBuffer cmd, uint32_t jobs, VkPipeline pipeline) const;
    void resetCounts(VkCommandBuffer cmd, uint32_t jobs) const;
    static void barrier(VkCommandBuffer cmd, VkPipelineStageFlags srcStage,
                        VkAccessFlags srcAccess, VkPipelineStageFlags dstStage,
                        VkAccessFlags dstAccess);
    VkDevice device_ = VK_NULL_HANDLE;
    GpuMesherBuffers buffers_{};
    VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    std::array<VkPipeline, 4> pipelines_{};
    uint32_t maxJobs_ = 0;
    uint32_t dispatchYLimit_ = 0;
    uint32_t finalizeXLimit_ = 0;
};

} // namespace OptiCraft::Vulkan
