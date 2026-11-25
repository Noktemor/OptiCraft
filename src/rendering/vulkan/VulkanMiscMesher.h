#pragma once
#include "rendering/vulkan/FrameVoxelDescriptors.h"
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <span>
#include "rendering/vulkan/GpuWorldCore.h"

namespace OptiCraft::Vulkan {

// Shader storage block; the order/offsets match voxel_misc_common.glsl.
// Indirect voxel ABI: source.x/y block/meta base or inline word;
// neighborBlocksB.z light base/word; links are stable slot IDs.
struct alignas(16) GpuMiscSlot {
    std::array<uint32_t,4> source{}; // blocksBase/word, metadataBase/word, quadBase, quadCapacity
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
    void recordCount(VkCommandBuffer command,uint32_t jobs, std::span<const uint8_t> kinds={}) const;
    void recordEmit(VkCommandBuffer command,uint32_t jobs, std::span<const uint8_t> kinds={}) const;
    void recordFastEmit(VkCommandBuffer command,uint32_t jobs,std::span<const uint8_t> kinds,
                        BufferRange slab,uint32_t capacity) const;
    void shutdown();
    VkDescriptorSetLayout descriptorSetLayout() const {return layout_;}
    VkDescriptorSet descriptorSet() const {return set_;}
    uint32_t maxJobs() const {return maxJobs_;}
    void selectVoxelFrame(uint32_t frame,uint64_t epoch,const std::array<VkBuffer,3>& buffers,
                          const std::array<VkDeviceSize,3>& sizes) {
        set_=frameDescriptors_.select(device_,frame,epoch,{0,1,10},buffers,sizes);
    }
private:
    void record(VkCommandBuffer command,uint32_t jobs,VkPipeline pipeline,std::span<const uint8_t> kinds,uint32_t fastBase=0,uint32_t fastCapacity=0) const;
    VkDevice device_=VK_NULL_HANDLE;
    VkDescriptorSetLayout layout_=VK_NULL_HANDLE;
    VkDescriptorPool pool_=VK_NULL_HANDLE;
    VkDescriptorSet set_=VK_NULL_HANDLE;
    FrameVoxelDescriptors frameDescriptors_;
    VkPipelineLayout pipelineLayout_=VK_NULL_HANDLE;
    std::array<VkPipeline,2> pipelines_{};
    uint32_t dispatchYLimit_=0, maxJobs_=0;
    GpuMiscBuffers buffers_{};
};
} // namespace OptiCraft::Vulkan
