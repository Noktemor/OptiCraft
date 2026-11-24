#pragma once

#include "rendering/vulkan/GpuMeshTransactions.h"
#include "rendering/vulkan/VulkanChunkStreamGpu.h"
#include "rendering/vulkan/VulkanGpuMesher.h"
#include "rendering/vulkan/VulkanMiscMesher.h"
#include "rendering/vulkan/VulkanMaterialTables.h"
#include <array>
#include <span>

namespace OptiCraft::Vulkan {

// Connects the Stage 5 real GPU block store to BOTH mesh compute passes.
// Performs GPU count -> fence/readback -> range allocation -> GPU emit ->
// fence/readback -> validated GPU publish; never creates CPU geometry.
// The current implementation serializes one bounded batch at a time so a
// single pair of counters and job-index buffers can be safely reused.
class VulkanChunkMeshing {
public:
    struct Config {
        uint64_t opaqueArenaBytes=32ull*1024*1024;
        uint64_t miscArenaBytes=64ull*1024*1024;
        uint32_t batchLimit=8;
    };
    struct Shaders {
        std::span<const uint32_t> opaqueCount, opaqueEmit, opaqueFinalize, opaquePublish;
        std::span<const uint32_t> miscCount, miscEmit;
    };
    VulkanChunkMeshing() = default;
    ~VulkanChunkMeshing() { shutdown(); }
    VulkanChunkMeshing(const VulkanChunkMeshing&)=delete;
    VulkanChunkMeshing& operator=(const VulkanChunkMeshing&)=delete;

    void initialize(VkPhysicalDevice physical, VkDevice device, VulkanChunkStreamGpu& stream,
                    const VulkanMaterialTables& materials, Config config, const Shaders& programs);
    // Called AFTER VulkanFrameRuntime::beginFrame (frame-slot fence waited)
    // and BEFORE stream.record(). Candidate geometry ranges will be uploaded
    // by stream.record() into the same primary command buffer this frame.
    void beginFrame(uint32_t frameSlot);
    // Called AFTER stream.record(primary,frameSlot); includes changed neighbors.
    // Count/Emit/Publish are all recorded before GPU culling/drawing.
    void record(VkCommandBuffer primary, uint32_t frameSlot, const StreamBatch& changed);
    void shutdown() noexcept; // owner waits all GPU submissions first
    VkBuffer opaqueFaces()const noexcept {return opaqueFaces_.buffer();}
    VkDeviceSize opaqueFaceBytes()const noexcept{return opaqueFaces_.size();}
    VkBuffer miscQuads()const noexcept {return miscQuads_.buffer();}
    VkBuffer miscPublishedDraws()const noexcept {return miscDraws_.buffer();}
    VkDescriptorSet opaqueSet()const noexcept {return opaque_.descriptorSet();}
    VkDescriptorSetLayout opaqueLayout()const noexcept {return opaque_.descriptorSetLayout();}
    VkDescriptorSet miscSet()const noexcept {return misc_.descriptorSet();}
    VkDescriptorSetLayout miscLayout()const noexcept {return misc_.descriptorSetLayout();}
    const GpuMeshTransactions::Stats& stats()const {return jobs_->stats();}

private:
    enum class Wait : uint8_t {None,Count,Emit,PublishReady};
    void uploadJobs(VkCommandBuffer command, std::span<const uint32_t> indices);
    void recordReadback(VkCommandBuffer command);
    std::vector<uint32_t> batchIndices(bool eligibleOnly) const;
    bool matching(const GpuMeshTransactions::Entry& entry)const;
    void barrier(VkCommandBuffer cmd, VkPipelineStageFlags from, VkAccessFlags read,
                 VkPipelineStageFlags to, VkAccessFlags write)const;
    VkPhysicalDevice physical_=VK_NULL_HANDLE;
    VkDevice device_=VK_NULL_HANDLE;
    VulkanChunkStreamGpu* stream_=nullptr;
    VulkanDeviceBuffer opaqueFaces_, miscQuads_, opaqueScratch_, miscScratch_;
    VulkanDeviceBuffer jobSlots_, opaqueCandidates_, readback_, miscDraws_;
    VulkanGpuMesher opaque_;
    VulkanMiscMesher misc_;
    std::unique_ptr<GpuMeshTransactions> jobs_;
    uint32_t batchLimit_=0, waitingSlot_=UINT32_MAX;
    Wait waiting_=Wait::None;
    std::vector<uint32_t> readOpaque_,readMisc_;
};

} // namespace OptiCraft::Vulkan
