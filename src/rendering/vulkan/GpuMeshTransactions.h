#pragma once

#include "rendering/vulkan/GpuWorldCore.h"
#include <cstdint>
#include <array>
#include <cstddef>
#include <deque>
#include <optional>
#include <span>
#include <vector>

namespace OptiCraft::Vulkan {

// GPU geometry transaction coordinator. CPU owns the arena, reserves bounded
// slabs for GPU suballocation or exact ranges after count readback on fallback.
// It never generates/inspects triangles, quads or faces. Publication requires
// validated candidate metadata and current slot generation/revision.
// Multiple independent batches share the same resident records and arenas.
// The render owner selects a batch with independent GPU scratch storage.
class GpuMeshTransactions {
public:
    enum class Phase : uint8_t { Idle, Counting, Emitting };
    struct Entry {
        uint32_t slot = UINT32_MAX;
        uint32_t generation = 0;
        uint64_t revision = 0;
        uint32_t opaqueCount = 0;
        uint32_t miscCount = 0;
        BufferRange opaqueCandidate{}, miscCandidate{};
        bool eligible = true;
        uint32_t workCost = 8;
        bool forceLegacy = false;
        bool interactive = false;
    };
    struct DrawChange {
        uint32_t slot = UINT32_MAX;
        uint32_t generation = 0;
        uint32_t opaqueCount = 0, miscCount = 0;
        uint32_t opaqueBase = 0, miscBase = 0;
        // True only for a candidate that passed BOTH GPU counts and revisions.
        bool valid = false;
    };
    // Same 16-byte ABI as VkDrawIndirectCommand, independent of Vulkan headers.
    using FastDraw = std::array<uint32_t,4>;
    static_assert(sizeof(FastDraw)==16);
    struct Stats {uint64_t published=0, stale=0, overflow=0, allocationFailures=0, allocationRetries=0, opaqueCacheHits=0, opaqueCacheOverflow=0, fastBatches=0, fastJobs=0, fastFallbacks=0, fastReservationMisses=0, fastMixedJobs=0, fastMiscOverflow=0, interactiveStarted=0, interactivePublished=0;};

    GpuMeshTransactions(uint32_t maxSlots, uint64_t opaqueBytes, uint64_t miscBytes,
                        uint32_t maxBatch = 8, uint32_t workBudget = 0, uint32_t batchCount = 1);
    void selectBatch(uint32_t index);
    bool isCurrent(const Entry& entry) const { return current(entry); }
    void dirty(uint32_t slot, uint32_t generation, uint32_t workCost = 8, bool interactive = false);
    void clearEmpty(uint32_t slot, uint32_t generation);
    void evict(uint32_t slot, uint32_t generation);
    [[nodiscard]] bool startCount();
    bool hasReadyInteractive() const noexcept;
    bool batchInteractive(uint32_t index) const noexcept;
    [[nodiscard]] Phase phase() const noexcept { return batch().phase; }
    [[nodiscard]] const std::vector<Entry>& jobs() const noexcept { return batch().jobs; }
    // Completes count on a frame fence. On failure, the previous published
    // geometry remains unchanged. Returns true when there are emit candidates.
    [[nodiscard]] bool finishCount(std::span<const uint32_t> opaqueCounts,
                                   std::span<const uint32_t> miscCounts);
    // CPU reserves bounded slabs; GPU assigns exact opaque ranges and copies cached
    // geometry, and directly emits misc into disjoint bounded job regions.
    // Complete only after that submission fence. A zero misc capacity is opaque-only.
    BufferRange reserveFastOpaque(uint32_t cacheCapacity, uint32_t miscCapacity=0);
    BufferRange fastMiscSlab() const noexcept {return batch().fastMiscSlab;}
    void finishFastOpaque(std::span<const uint32_t> counts, std::span<const FastDraw> draws,
                          std::span<const uint32_t> miscCounts={});
    // Called only when the emit submission's fence has completed. Returns
    // per-job draw updates; the caller writes GPU commands BEFORE acceptPublish.
    [[nodiscard]] std::vector<DrawChange> verifyEmit(std::span<const uint32_t> opaqueCounts,
                                                      std::span<const uint32_t> miscCounts);
    void acceptPublish(std::span<const DrawChange> accepted);
    // Each completed frame-slot fence advances this counter. The renderer must
    // call only after beginFrame has waited that frame slot's fence.
    void completedFrame();
    void recordOpaqueCacheDispatch(bool cached) noexcept {if(cached)++stats_.opaqueCacheHits;else ++stats_.opaqueCacheOverflow;}
    void cancelInFlight();
    [[nodiscard]] uint32_t maxBatch() const noexcept { return maxBatch_; }
    void setWorkBudget(uint32_t budget) noexcept { workBudget_=budget?budget:1u; }
    bool hasPending() const noexcept {return pendingCount_!=0;}
    size_t pendingCount() const noexcept;
    size_t inFlightCount() const noexcept {
        size_t count=0;for(const auto& b:batches_)count+=b.jobs.size();return count;
    }
    uint32_t batchWork(bool eligibleOnly) const noexcept;
    [[nodiscard]] uint64_t opaqueUsed() const noexcept { return opaque_.used(); }
    [[nodiscard]] uint64_t miscUsed() const noexcept { return misc_.used(); }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    struct Resident {
        uint32_t generation = 0;
        uint64_t revision = 0;
        bool active = false;
        bool pending = false;
        bool interactive = false;
        bool retryOnRelease = false;
        bool inFlight = false;
        bool forceLegacy = false;
        uint32_t workCost = 8;
        uint64_t pendingToken=0;
        BufferRange opaque{}, misc{};
    };
    struct Deferred {BufferRange opaque{}, misc{}; uint64_t readyFrame=0;};
    bool current(const Entry& entry) const;
    void defer(BufferRange opaque, BufferRange misc);
    void enqueue(uint32_t slot);
    void cancelPending(Resident& slot) noexcept;
    void discard(Entry& entry);
    std::vector<Resident> slots_;
    struct Pending {uint32_t slot,generation; uint64_t token;};
    std::deque<Pending> pending_, interactivePending_;
    uint64_t nextPendingToken_=0;
    size_t pendingCount_=0;
    struct Batch { std::vector<Entry> jobs; Phase phase=Phase::Idle; BufferRange fastSlab{}, fastMiscSlab{}; uint32_t fastCapacity=0, fastMiscCapacity=0; };
    std::vector<Batch> batches_;
    uint32_t selectedBatch_=0;
    Batch& batch() noexcept {return batches_[selectedBatch_];}
    const Batch& batch() const noexcept {return batches_[selectedBatch_];}
    void releaseBatch();
    std::vector<Deferred> retirement_;
    RangeArena opaque_, misc_;
    uint32_t maxBatch_ = 0, workBudget_ = 0;
    uint64_t completedFrames_ = 0;
    bool reclaimedSinceFrame_=false;
    Stats stats_{};
};

} // namespace OptiCraft::Vulkan
