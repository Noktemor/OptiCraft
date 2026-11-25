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
// Fast batches: GPU opaque count/cache copy + bounded direct misc emit
// -> fence/readback -> validated publish of both geometry classes. Fallback
// batches use exact host allocation between Count and Emit. No CPU geometry.
// Two independent scratch batches share persistent geometry arenas. At most
// one budgeted mesh phase (including combined fast mesh) per graphics frame.
class VulkanChunkMeshing {
public:
    struct Config {
        uint64_t opaqueArenaBytes=32ull*1024*1024;
        uint64_t miscArenaBytes=64ull*1024*1024;
        uint32_t batchLimit=8;
        uint32_t workBudget=0; // estimated shader work units, 0 = legacy batch limit
        uint32_t opaqueCacheRects=8192; // per-job packed rectangles, 0 disables
        bool coarseCache=true; // per-job LOD2 classification scratch
        bool planeCompaction=true; // exact visibility rows + indirect plane list
        bool fastOpaque=true; // bounded GPU suballocation
        uint32_t fastMiscQuads=4096; // per-job reservation, 0 disables mixed fast path
        uint32_t activeBatches=2; // diagnostic override: 1
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
    bool acceptsUploads() const noexcept;
    // After stream.record(), before any bind/dispatch of the current frame.
    void selectVoxelFrame(uint32_t frameSlot);
    // Called AFTER stream.record(primary,frameSlot); includes changed neighbors.
    // Count/Emit/Publish are all recorded before GPU culling/drawing.
    uint32_t record(VkCommandBuffer primary, uint32_t frameSlot, const StreamBatch& changed);
    bool mayDispatch(const StreamBatch& changed) const noexcept;
    bool lastDispatchWasCount() const noexcept {return lastDispatchWasCount_;}
    void setWorkBudget(uint32_t budget) noexcept {jobs_->setWorkBudget(budget);}
    size_t pendingCount() const noexcept {return jobs_->pendingCount();}
    size_t inFlightCount() const noexcept {return jobs_->inFlightCount();}
    void shutdown() noexcept; // owner waits all GPU submissions first
    VkBuffer opaqueFaces()const noexcept {return opaqueFaces_.buffer();}
    VkDeviceSize opaqueFaceBytes()const noexcept{return opaqueFaces_.size();}
    VkBuffer miscQuads()const noexcept {return miscQuads_.buffer();}
    VkDeviceSize miscQuadBytes()const noexcept {return miscQuads_.size();}
    VkBuffer miscPublishedDraws()const noexcept {return miscDraws_.buffer();}
    VkDescriptorSet opaqueSet()const noexcept {return batches_[0].opaque.descriptorSet();}
    VkDescriptorSetLayout opaqueLayout()const noexcept {return batches_[0].opaque.descriptorSetLayout();}
    VkDescriptorSet miscSet()const noexcept {return batches_[0].misc.descriptorSet();}
    VkDescriptorSetLayout miscLayout()const noexcept {return batches_[0].misc.descriptorSetLayout();}
    const GpuMeshTransactions::Stats& stats()const {return jobs_->stats();}

private:
    enum class Wait : uint8_t {None,Count,Emit,FastOpaque,PublishReady};
    static constexpr uint32_t kBatchCount=2;
    struct Batch {
        VulkanDeviceBuffer opaqueScratch,miscScratch,jobSlots,opaqueCandidates,readback,opaqueCache;
        VulkanGpuMesher opaque;
        VulkanMiscMesher misc;
        uint32_t waitingSlot=UINT32_MAX;
        Wait waiting=Wait::None;
        bool emitReady=false, fastMixed=false;
        std::vector<uint32_t> readOpaque,readMisc;
        std::vector<GpuMeshTransactions::FastDraw> readFastDraws;
    };
    void uploadJobs(Batch& batch,VkCommandBuffer command, std::span<const uint32_t> indices);
    void recordReadback(Batch& batch,VkCommandBuffer command);
    std::vector<uint32_t> batchIndices(bool eligibleOnly) const;
    std::vector<uint8_t> batchKinds(std::span<const uint32_t> ids) const;
    bool matching(const GpuMeshTransactions::Entry& entry)const;
    void barrier(VkCommandBuffer cmd, VkPipelineStageFlags from, VkAccessFlags read,
                 VkPipelineStageFlags to, VkAccessFlags write)const;
    VkPhysicalDevice physical_=VK_NULL_HANDLE;
    VkDevice device_=VK_NULL_HANDLE;
    VulkanChunkStreamGpu* stream_=nullptr;
    VulkanDeviceBuffer opaqueFaces_, miscQuads_, miscDraws_;
    std::array<Batch,kBatchCount> batches_;
    std::unique_ptr<GpuMeshTransactions> jobs_;
    bool lastDispatchWasCount_=false, fastOpaque_=true;
    uint32_t batchLimit_=0, nextBatch_=0, activeBatches_=0, opaqueCacheRects_=0, fastMiscQuads_=0;
};

} // namespace OptiCraft::Vulkan
