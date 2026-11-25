#pragma once
#include "rendering/vulkan/FrameVoxelDescriptors.h"

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
// With origin.w bit 5 set, neighbour links are stable slot IDs (-1 absent),
// neighborB.z/w hold meta/light bases or inline words, and bits 6..8 mark
// inline block/meta/light. Legacy probes retain direct dense offsets.
struct alignas(16) GpuChunkSlot {
    std::array<uint32_t, 4> source{}; // blockBase/word, faceBase, faceCapacity, flags
    std::array<int32_t, 4> origin{};   // world-space origin
    std::array<int32_t, 4> neighborA{{-1, -1, -1, -1}};
    std::array<int32_t, 4> neighborB{{-1, -1, -1, -1}};
};
static_assert(sizeof(GpuChunkSlot) == 64 && alignof(GpuChunkSlot) == 16);

// Device buffers must be allocated/bound by the renderer, and stay alive during
// dispatch. Transfer/upload ownership, Vulkan queue synchronization, budget
// control, readback and old-mesh COW publication live in the caller.
struct GpuMesherBuffers {
    std::array<VkBuffer, 11> buffers{};
    std::array<VkDeviceSize, 11> byteSizes{};
    // bindings: 0 block words, 1 stable chunk slots, 2 per-batch counters,
    // 3 packed faces, 4 65536 block flags, 5 persistent indirect draws,
    // 6 per-batch stable slot indices, 7 candidate indirect draws,
    // 8 packed byte metadata, 9 packed light bytes, 10 batch-private rectangle cache.
    uint32_t cacheRectsPerJob=0;
    bool coarseCache=false; // optional 4096 uints/job after rectangle cache
    bool planeCompaction=false; // optional indirect arguments, list, visibility rows
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
    // The exact fallback uses separate Count/Emit submissions. The fast opaque
    // path suballocates from a bounded slab and copies Count cache immediately.
    void recordCount(VkCommandBuffer cmd, uint32_t jobCount, std::span<const uint8_t> kinds={}) const;
    void recordEmit(VkCommandBuffer cmd, uint32_t jobCount, std::span<const uint8_t> kinds={}) const;
    void recordFastOpaque(VkCommandBuffer cmd, uint32_t jobCount, BufferRange slab) const;
    // Only after emit completes and ALL candidates in this batch are validated.
    // No published draw is touched by count/emit/finalize.
    void recordPublish(VkCommandBuffer cmd, uint32_t jobCount) const;
    VkDescriptorSetLayout descriptorSetLayout() const { return descriptorLayout_; }
    VkDescriptorSet descriptorSet() const { return descriptorSet_; }
    uint32_t maxJobs() const { return maxJobs_; }
    void selectVoxelFrame(uint32_t frame,uint64_t epoch,const std::array<VkBuffer,3>& buffers,
                          const std::array<VkDeviceSize,3>& sizes) {
        descriptorSet_=frameDescriptors_.select(device_,frame,epoch,{0,8,9},buffers,sizes);
    }
private:
    struct Params { uint32_t firstJob, totalJobs, cacheCapacity, fastBasePlusOne=0; };
    void dispatchMeshes(VkCommandBuffer cmd, uint32_t jobs, VkPipeline pipeline, std::span<const uint8_t> kinds) const;
    void recordCoarseCache(VkCommandBuffer cmd,uint32_t jobs,std::span<const uint8_t> kinds) const;
    void recordPlaneCompaction(VkCommandBuffer cmd,uint32_t jobs,std::span<const uint8_t> kinds) const;
    void resetCounts(VkCommandBuffer cmd, uint32_t jobs) const;
    static void barrier(VkCommandBuffer cmd, VkPipelineStageFlags srcStage,
                        VkAccessFlags srcAccess, VkPipelineStageFlags dstStage,
                        VkAccessFlags dstAccess);
    VkDevice device_ = VK_NULL_HANDLE;
    GpuMesherBuffers buffers_{};
    VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    FrameVoxelDescriptors frameDescriptors_;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    std::array<VkPipeline, 4> pipelines_{};
    uint32_t maxJobs_ = 0;
    uint32_t dispatchYLimit_ = 0;
    uint32_t finalizeXLimit_ = 0;
};

} // namespace OptiCraft::Vulkan
