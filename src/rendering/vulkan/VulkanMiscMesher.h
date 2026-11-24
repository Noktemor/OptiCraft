#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <span>

namespace OptiCraft::Vulkan {

// Shader storage block; the order/offsets match voxel_misc_common.glsl.
struct alignas(16) GpuMiscSlot {
    std::array<uint32_t,4> source{}; // blocksBase, metadataBase, quadBase, quadCapacity
    std::array<int32_t,4> origin{};
    std::array<int32_t,4> neighborBlocksA{{-1,-1,-1,-1}};
    std::array<int32_t,4> neighborBlocksB{{-1,-1,-1,-1}};
    std::array<int32_t,4> neighborMetaA{{-1,-1,-1,-1}};
    std::array<int32_t,4> neighborMetaB{{-1,-1,-1,-1}};
};
static_assert(sizeof(GpuMiscSlot)==96);

// All buffers are persistent, bound and owned by the caller. In binding 7 each
// output quad occupies TWO uvec4 records (32 bytes). Count/readback/allocate/
// emit/validate/publish are separate async phases (no partially drawn meshes).
struct GpuMiscBuffers {
    std::array<VkBuffer,11> buffers{};
    std::array<VkDeviceSize,11> byteSizes{};
    // 0 blocks, 1 metadata, 2 slots, 3 materials, 4 shapeIndex, 5 shapeBoxes,
    // 6 counters, 7 misc quads, 8 batch slot IDs, 9 atlas UVs, 10 packed block/sky light.
};

class VulkanMiscMesher {
public:
    ~VulkanMiscMesher();
    VulkanMiscMesher(const VulkanMiscMesher&)=delete;
    VulkanMiscMesher& operator=(const VulkanMiscMesher&)=delete;
    VulkanMiscMesher()=default;
    void initialize(VkPhysicalDevice physical,VkDevice device,const GpuMiscBuffers& buffers,
                    std::span<const uint32_t> countSpv,std::span<const uint32_t> emitSpv);
    void recordCount(VkCommandBuffer command,uint32_t jobs) const;
    void recordEmit(VkCommandBuffer command,uint32_t jobs) const;
    void shutdown();
    VkDescriptorSetLayout descriptorSetLayout() const {return layout_;}
    VkDescriptorSet descriptorSet() const {return set_;}
    uint32_t maxJobs() const {return maxJobs_;}
private:
    void record(VkCommandBuffer command,uint32_t jobs,VkPipeline pipeline) const;
    VkDevice device_=VK_NULL_HANDLE;
    VkDescriptorSetLayout layout_=VK_NULL_HANDLE;
    VkDescriptorPool pool_=VK_NULL_HANDLE;
    VkDescriptorSet set_=VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_=VK_NULL_HANDLE;
    std::array<VkPipeline,2> pipelines_{};
    uint32_t dispatchYLimit_=0, maxJobs_=0;
    GpuMiscBuffers buffers_{};
};
} // namespace OptiCraft::Vulkan
