#pragma once

#include "rendering/vulkan/GpuWorldCore.h"
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

namespace OptiCraft::Vulkan {

// GPU geometry transaction coordinator. CPU only allocates byte ranges after
// reading GPU *counts*; it never generates/inspects triangles, quads or faces.
// A slot is not published until both GPU emitted counts match its count pass
// and its generation/revision still agrees with the current world snapshot.
// One global Count/Emit batch in flight; GPU scratch SSBOs therefore cannot
// race a second compute submission using the same scratch/dispatch arrays.
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
    };
    struct DrawChange {
        uint32_t slot = UINT32_MAX;
        uint32_t generation = 0;
        uint32_t opaqueCount = 0, miscCount = 0;
        uint32_t opaqueBase = 0, miscBase = 0;
        // True only for a candidate that passed BOTH GPU counts and revisions.
        bool valid = false;
    };
    struct Stats {uint64_t published=0, stale=0, overflow=0, allocationFailures=0;};

    GpuMeshTransactions(uint32_t maxSlots, uint64_t opaqueBytes, uint64_t miscBytes,
                        uint32_t maxBatch = 8);
    void dirty(uint32_t slot, uint32_t generation);
    void evict(uint32_t slot, uint32_t generation);
    [[nodiscard]] bool startCount();
    [[nodiscard]] Phase phase() const noexcept { return phase_; }
    [[nodiscard]] const std::vector<Entry>& jobs() const noexcept { return jobs_; }
    // Completes count on a frame fence. On failure, the previous published
    // geometry remains unchanged. Returns true when there are emit candidates.
    [[nodiscard]] bool finishCount(std::span<const uint32_t> opaqueCounts,
                                   std::span<const uint32_t> miscCounts);
    // Called only when the emit submission's fence has completed. Returns
    // per-job draw updates; the caller writes GPU commands BEFORE acceptPublish.
    [[nodiscard]] std::vector<DrawChange> verifyEmit(std::span<const uint32_t> opaqueCounts,
                                                      std::span<const uint32_t> miscCounts);
    void acceptPublish(std::span<const DrawChange> accepted);
    // Each completed frame-slot fence advances this counter. The renderer must
    // call only after beginFrame has waited that frame slot's fence.
    void completedFrame();
    void cancelInFlight();
    [[nodiscard]] uint32_t maxBatch() const noexcept { return maxBatch_; }
    [[nodiscard]] uint64_t opaqueUsed() const noexcept { return opaque_.used(); }
    [[nodiscard]] uint64_t miscUsed() const noexcept { return misc_.used(); }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    struct Resident {
        uint32_t generation = 0;
        uint64_t revision = 0;
        bool active = false;
        bool pending = false;
        BufferRange opaque{}, misc{};
    };
    struct Deferred {BufferRange opaque{}, misc{}; uint64_t readyFrame=0;};
    bool current(const Entry& entry) const;
    void defer(BufferRange opaque, BufferRange misc);
    void discard(Entry& entry);
    std::vector<Resident> slots_;
    std::deque<uint32_t> pending_;
    std::vector<Entry> jobs_;
    std::vector<Deferred> retirement_;
    RangeArena opaque_, misc_;
    uint32_t maxBatch_ = 0;
    uint64_t completedFrames_ = 0;
    Phase phase_ = Phase::Idle;
    Stats stats_{};
};

} // namespace OptiCraft::Vulkan
