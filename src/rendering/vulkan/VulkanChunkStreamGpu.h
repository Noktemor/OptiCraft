#pragma once

#include "rendering/vulkan/VulkanChunkStreamPlan.h"
#include "rendering/vulkan/VulkanDeviceBuffer.h"
#include "rendering/vulkan/VoxelChannelPages.h"
#include "rendering/vulkan/VulkanGpuMesher.h"
#include "rendering/vulkan/OverlaySnapshot.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace OptiCraft::Vulkan {

// Stage 5: real, persistent Vulkan allocation + GPU transfer of original block
// state from Game. GPU compute mesher consumes these buffers directly.
// Single graphics+compute queue only; caller must record between beginFrame and
// endColorPass, and call record AFTER VulkanFrameRuntime::beginFrame waits the
// frame fence. No device-idle, VkAllocateMemory or vkCreateBuffer per chunk.
class VulkanChunkStreamGpu {
public:
    static constexpr uint32_t kFrameSlots = 2;
    struct Config {
        uint32_t maxResident = 512;
        uint32_t uploadsPerFrame = 8;
        uint32_t maxQueued = 1024;
        bool indirectVoxelPages = true;
        bool growVoxelArenas = true;
        std::array<uint32_t,3> initialPages{256,64,256};
        // Number of consecutive idle, low-occupancy records before trimming.
        // Zero disables shrinking; growth remains independent.
        uint32_t shrinkDelayFrames=120;
    };
    VulkanChunkStreamGpu() = default;
    ~VulkanChunkStreamGpu() { shutdown(); }
    VulkanChunkStreamGpu(const VulkanChunkStreamGpu&) = delete;
    VulkanChunkStreamGpu& operator=(const VulkanChunkStreamGpu&) = delete;

    void initialize(VkPhysicalDevice physical, VkDevice device, Config config);
    // Ordinary packets are safe for producer threads. Packets with a GPU
    // lease must be submitted on the graphics owner thread.
    void submit(std::vector<LiveChunkPacket>&& packets);
    // Graphics owner thread only. Requires a ready same-device snapshot and
    // supplied authoritative light. Returns false on unavailable snapshot;
    // no queue mutation on failure. Producer owns classification/revisions.
    bool submitGpuSnapshot(LiveChunkPacket&& packet);
    // Exclusive GPU-world admission, NOT the ordinary CPU mirror snapshot path.
    // Graphics/world owner thread only; caller must first establish absence of
    // CPU jobs/objects/saves and exclude CPU world producers for this key.
    // World producers should use OwnedChunkPromotion.h to establish that claim
    // before generation and to retain it until stream retirement.
    struct OwnershipToken {
        const VulkanChunkStreamGpu* owner=nullptr;
        ChunkKey key{};
        uint64_t serial=0;
        explicit operator bool() const noexcept {return owner && serial;}
        friend bool operator==(const OwnershipToken&,const OwnershipToken&)=default;
    };
    struct PromotionToken {
        OwnershipToken source;
        uint64_t attempt=0,cpuTicket=0;
        explicit operator bool() const noexcept {return bool(source) && attempt && cpuTicket;}
        friend bool operator==(const PromotionToken&,const PromotionToken&)=default;
    };
    enum class OwnershipState {Queued,Resident,Promoting,Retiring};
    std::optional<OwnershipToken> submitOwnedGpuSnapshot(LiveChunkPacket&& packet);
    std::optional<OwnershipState> ownershipState(OwnershipToken token) const;
    // Committed uniform block page only; dense/unknown pages never certify occlusion.
    bool uniformSkyOccluder(ChunkKey key,std::span<const uint32_t> skyBits) const;
    // Also accepts a dense page with a certified opaque top plane.
    bool skyOccluder(ChunkKey key,std::span<const uint32_t> skyBits) const;
    // Read-only scheduler query. Includes promoting/retiring owners.
    std::optional<OwnershipState> ownedStateAt(ChunkKey key) const;
    std::optional<PromotionToken> beginOwnedPromotion(OwnershipToken token,uint64_t cpuTicket);
    bool cancelOwnedPromotion(PromotionToken token);
    // Caller certifies reproducibility or durable persistence before discarding.
    // Keep the key protected until record() has retired the old slot.
    bool discardOwnedChunk(OwnershipToken token);
    void evict(ChunkKey key);
    // GPU idle, producers detached, only immutable procedural sources (or saved sources).
    void retireOwnedIdle();
    // Connect these directly to Game::set_vulkan_chunk_sink(). The GPU store
    // must outlive the callbacks; unregister before destroying it.
    VulkanGameChunkFeed::UploadSink uploadSink();
    VulkanGameChunkFeed::EvictSink evictSink();
    VulkanGameChunkFeed::SnapshotLeaseSink snapshotLeaseSink(); // graphics owner thread only
    // Record one batched transfer into the frame's primary command buffer.
    // Returns dirty slot IDs to enqueue for GPU count/emit meshing. No work
    // submitted when inputs are unchanged. Returns frame-local data by value.
    StreamBatch record(VkCommandBuffer primary, uint32_t frameSlot, bool allowUploads=true);
    bool currentStreamBatch(const StreamBatch& batch) const noexcept {
        return planner_ && lightFrameCmd_ && batch.streamOwner==this &&
               batch.streamRecordSerial==lightFrameSerial_;
    }
    bool currentStreamFrame(VkCommandBuffer cmd,uint32_t slot,const StreamBatch& batch) const noexcept {
        return currentStreamBatch(batch) && cmd==lightFrameCmd_ && slot==lightFrameSlot_;
    }
    // Owner thread, after record() and all lighting writes, before render pass.
    // One outstanding snapshot per frame slot. record() on the same slot
    // (after its fence) makes the historical snapshot available for polling.
    struct ResidentReadback {
        const VulkanChunkStreamGpu* owner=nullptr;
        StreamResident source;
        uint64_t dataSerial=0;
        std::array<std::vector<uint32_t>,3> words; // packed block/meta/light; uniform = one word
        PromotionToken promotion; // nonempty only for an explicitly owned capture
    };
    bool recordResidentReadback(VkCommandBuffer cmd,uint32_t frameSlot,ChunkHandle handle);
    std::optional<ResidentReadback> takeResidentReadback(uint32_t frameSlot);
    // Do not consume another client's snapshot, including an old canceled attempt.
    std::optional<ResidentReadback> takeOwnedReadback(uint32_t frameSlot,PromotionToken token);
    // Validate immediately before owner-thread adoption. Pending producer
    // updates and authoritative CPU/world ownership are separate concerns.
    bool residentReadbackCurrent(const ResidentReadback& result) const;
    // Conservative promotion gate: also reject queued updates/evictions.
    // Caller must exclude concurrent producers for the ownership transition.
    bool residentReadbackQuiescent(const ResidentReadback& result) const;
    bool recordOwnedReadback(VkCommandBuffer cmd,uint32_t frameSlot,PromotionToken token);
    // Commit runs on the owner thread, synchronously. Callback may validate the
    // snapshot, but must not re-enter ownership-changing APIs. False retains
    // the reservation and authority for retry/cancel. Success schedules retirement.
    bool commitOwnedPromotion(PromotionToken token,const ResidentReadback& snapshot,
                              const std::function<bool()>& commitCpu);
    // Geometry owner updates these before the next record, after allocating the
    // corresponding persistent GPU ranges. They MUST match the current generation.
    void setOpaqueFaceRange(uint32_t slot, uint32_t generation, uint32_t base, uint32_t capacity);
    void setMiscQuadRange(uint32_t slot, uint32_t generation, uint32_t base, uint32_t capacity);
    void shutdown() noexcept; // must be called after all GPU submissions complete

    VkBuffer blocks() const noexcept { return blockWords_.buffer(); }
    VkBuffer metadata() const noexcept { return metaWords_.buffer(); }
    VkBuffer lights() const noexcept { return lightWords_.buffer(); }
    std::array<VkBuffer,3> voxelBuffers() const noexcept {return {blocks(),metadata(),lights()};}
    std::array<VkDeviceSize,3> voxelSizes() const noexcept {return {blockWords_.size(),metaWords_.size(),lightWords_.size()};}
    uint64_t voxelEpoch() const noexcept {return voxelEpoch_;}
    uint64_t voxelAllocatedBytes() const noexcept;
    uint64_t voxelGrowthCount() const noexcept {return growthCount_;}
    uint64_t voxelShrinkCount() const noexcept {return shrinkCount_;}
    uint64_t voxelAdmissionMisses() const noexcept {return admissionMisses_;}
    VkBuffer opaqueSlots() const noexcept { return opaqueSlots_.buffer(); }
    VkBuffer miscSlots() const noexcept { return miscSlots_.buffer(); }
    VkBuffer publishedDraws() const noexcept { return publishedDraws_.buffer(); }
    VkBuffer liveSlotIndices() const noexcept { return liveSlots_.buffer(); }
    uint32_t liveSlotCount() const noexcept { return liveSlotCount_; }
    struct UploadStats {uint64_t copiedBytes=0, filledBytes=0, reusedBytes=0, inlineBytes=0, migratedBytes=0, overlayReusedBytes=0;};
    const UploadStats& uploadStats() const noexcept {return uploadStats_;}
    uint64_t denseVoxelBytes() const noexcept {
        if(!config_.indirectVoxelPages)return uint64_t(liveSlotCount_)*131072u;
        return uint64_t(pages_[0].densePages())*65536u+
            uint64_t(pages_[1].densePages()+pages_[2].densePages())*32768u;
    }
    bool indirectVoxelPages() const noexcept {return config_.indirectVoxelPages;}
    const VulkanChunkStreamPlan& plan() const { return *planner_; }
private:
    friend class VulkanResidentLight;
    struct OwnedEntry {
        OwnershipToken token;
        uint64_t instance=0;
        PromotionToken promotion;
        bool retiring=false;
    };
    // This lock serializes admission with ordinary producer submissions, not
    // CPU-world generation. All ownership transitions and record() are owner-thread.
    mutable std::mutex ownershipMutex_;
    std::unordered_map<ChunkKey,OwnedEntry,ChunkKeyHash> owned_;
    inline static std::atomic<uint64_t> ownershipSerial_{0}; // process-wide, survives object replacement
    static uint64_t allocateOwnershipSerial() noexcept;
    bool matchesOwnedResident(const OwnedEntry& entry) const;

    // Same-command snapshot: all source reads must be recorded after record()
    // and before the next stream record. Subsequent same-queue transfers order
    // against these reads; migrated buffers survive their migration fence.
    struct LightFrameSnapshot {
        uint64_t serial=0;
        std::vector<GpuChunkSlot> slots;
        std::vector<uint32_t> core,selected;
    };
    bool currentLightFrame(VkCommandBuffer cmd,uint32_t frameSlot,uint64_t serial) const noexcept;
    std::optional<LightFrameSnapshot> lightFrameSnapshot(VkCommandBuffer cmd,uint32_t frameSlot,
                    std::span<const ChunkHandle> core,uint32_t maxJobs,
                    std::optional<int32_t> skyCeiling=std::nullopt,bool coreOnly=false,
                    bool partialHalo=false,std::span<const uint32_t> skyBits={}) const;
    VkCommandBuffer lightFrameCmd_=VK_NULL_HANDLE;
    uint32_t lightFrameSlot_=UINT32_MAX;
    uint64_t lightFrameSerial_=0;
    struct ReadbackFrame {
        VulkanDeviceBuffer buffer;
        std::optional<ResidentReadback> snapshot;
        bool ready=false;
    };
    std::array<ReadbackFrame,kFrameSlots> readbacks_;
    std::vector<uint64_t> dataSerials_;
    uint64_t nextDataSerial_=0; // do not reset on shutdown/reinitialize
    struct FaceRange {uint32_t generation=0,base=0,count=0;};
    struct StagingFrame { VulkanDeviceBuffer buffer; };
    struct RepeatedChannel {uint32_t word=0;bool known=false;};
    struct Fill {VkDeviceSize offset,size;uint32_t word;};
    struct RangeCopies {
        std::vector<VkBufferCopy> blocks, meta, light, opaque, misc, live;
    };
    bool growChannel(size_t kind,uint32_t needed,uint32_t frameSlot);
    void tryShrinkChannels(uint32_t frameSlot,bool idle);
    VkPhysicalDevice physical_=VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    Config config_{};
    uint32_t liveSlotCount_ = 0;
    std::unique_ptr<VulkanChunkStreamPlan> planner_;
    VulkanDeviceBuffer blockWords_, metaWords_, lightWords_;
    VulkanDeviceBuffer opaqueSlots_, miscSlots_, publishedDraws_, liveSlots_;
    std::array<StagingFrame,kFrameSlots> staging_;
    std::array<std::vector<std::shared_ptr<OverlaySnapshot>>,kFrameSlots> overlayLeases_;
    std::vector<FaceRange> opaqueFaces_, miscFaces_;
    std::vector<std::array<RepeatedChannel,3>> repeated_;
    std::vector<SkyRoofPalette> skyRoofs_;
    std::array<VoxelChannelPages,3> pages_;
    // Old buffers remain alive through the fence of the migration submission.
    std::array<std::array<VulkanDeviceBuffer,3>,kFrameSlots> retiredVoxels_;
    std::array<uint32_t,3> maxPages_{},retryFrames_{},idleFrames_{};
    uint64_t voxelEpoch_=0,growthCount_=0,shrinkCount_=0,admissionMisses_=0;
    UploadStats uploadStats_{};
    std::vector<uint32_t> dirtyGeometry_;
    std::vector<std::byte> stagingBytes_;
};

} // namespace OptiCraft::Vulkan
