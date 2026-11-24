#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

// Vulkan-independent world residency / work scheduling. No GL and no Vulkan calls.
// All GPU buffer offsets are in bytes. Device work and transfers are handled by the backend.
namespace OptiCraft::Vulkan {

inline constexpr uint32_t kChunkSide = 32;
inline constexpr uint32_t kChunkVoxels = kChunkSide * kChunkSide * kChunkSide;
inline constexpr uint32_t kChunkBlockWords = kChunkVoxels / 2;
inline constexpr uint32_t kMaxPackedFaceSlot = (1u << 16) - 1u;

// CPU-authoritative blocks only: export original block IDs, never geometry.
void packBlockIds(std::span<const uint16_t> voxels, std::span<uint32_t> words);

struct ChunkKey {
    int32_t dimension = 0;
    int32_t x = 0, y = 0, z = 0;
    friend bool operator==(const ChunkKey&, const ChunkKey&) = default;
};

struct ChunkKeyHash {
    size_t operator()(const ChunkKey& k) const noexcept;
};

struct ChunkHandle {
    uint32_t slot = UINT32_MAX;
    uint32_t generation = 0;
    explicit operator bool() const noexcept { return slot != UINT32_MAX && generation != 0; }
    friend bool operator==(const ChunkHandle&, const ChunkHandle&) = default;
};

struct BufferRange {
    uint64_t offset = 0;
    uint64_t size = 0;
    explicit operator bool() const noexcept { return size != 0; }
};

// Best-fit range allocator with adjacent free-range merging. Does not create VkBuffer/VkMemory.
class RangeArena {
public:
    explicit RangeArena(uint64_t capacity = 0);
    std::optional<BufferRange> allocate(uint64_t bytes, uint64_t alignment = 16);
    bool release(BufferRange range); // only a complete live allocation may be released
    bool owns(BufferRange range) const;
    uint64_t capacity() const { return capacity_; }
    uint64_t used() const { return used_; }
    uint64_t available() const { return capacity_ - used_; }
    size_t freeFragments() const { return free_.size(); }
private:
    uint64_t capacity_ = 0, used_ = 0;
    std::map<uint64_t, uint64_t> free_; // offset -> length
    std::map<uint64_t, uint64_t> allocated_; // prevent foreign/stale/subrange releases
};

enum class Authority : uint8_t { GpuProcedural, CpuAuthoritative };
enum class ChunkPhase : uint8_t {
    NeedsGpuGeneration, NeedsCpuUpload, Generating, Uploading, Resident,
    PromotionReadback, EvictionReadback, Retiring
};

struct ChunkEntry {
    ChunkKey key{};
    ChunkHandle handle{};
    Authority authority = Authority::GpuProcedural;
    ChunkPhase phase = ChunkPhase::NeedsGpuGeneration;
    BufferRange blockData{};
    BufferRange meshData{};
    uint64_t publishedVersion = 0;
    uint64_t readyTimeline = 0;
    uint32_t lodMask = 0;
};

// Main/streaming owner thread mutates registry. Completed timeline values are provided
// externally after observing vkGetSemaphoreCounterValue; no device-idle waits.
class ChunkRegistry {
public:
    ChunkRegistry(uint64_t blockBytes, uint64_t meshBytes);
    ChunkHandle ensure(ChunkKey key, Authority authority);
    ChunkEntry* resolve(ChunkHandle handle);
    const ChunkEntry* resolve(ChunkHandle handle) const;
    ChunkHandle lookup(ChunkKey key) const;
    bool setBlocks(ChunkHandle handle, BufferRange bytes, uint64_t readyTimeline);
    // Copy-on-write. Old geometry is never reclaimed before retireTimeline.
    bool publishMesh(ChunkHandle handle, BufferRange mesh, uint64_t version,
                     uint64_t readyTimeline, uint64_t retireTimeline);
    bool requestPromotion(ChunkHandle handle);
    bool finishPromotion(ChunkHandle handle); // after CPU has imported/read back data
    bool requestEviction(ChunkHandle handle);
    bool retire(ChunkHandle handle, uint64_t lastUseTimeline);
    void collect(uint64_t completedTimeline);
    RangeArena& blocks() { return blocks_; }
    RangeArena& meshes() { return meshes_; }
    const RangeArena& blocks() const { return blocks_; }
    const RangeArena& meshes() const { return meshes_; }
    size_t liveCount() const { return index_.size(); }
private:
    struct Slot { ChunkEntry entry{}; uint32_t generation = 0; bool live = false; };
    struct Deferred { uint64_t timeline; BufferRange block{}, mesh{}; uint32_t slot = UINT32_MAX; };
    std::vector<Slot> slots_;
    std::vector<uint32_t> reusableSlots_;
    std::unordered_map<ChunkKey, ChunkHandle, ChunkKeyHash> index_;
    std::vector<Deferred> pendingRetire_;
    RangeArena blocks_, meshes_;
};

enum class WorkKind : uint8_t {
    GenerateFullChunk, UploadCpuChunk, CountMesh, EmitMesh, BuildLod,
    PromoteReadback, EvictReadback
};

struct GpuWork {
    ChunkHandle handle{};
    WorkKind kind = WorkKind::GenerateFullChunk;
    uint64_t version = 0;
    uint32_t estimatedGroups = 1;
    uint32_t distanceSquared = 0;
    uint8_t priority = 0; // larger is more urgent
};

// Multiple producers submit arrays. One consumer drains once per render frame.
// Coalesces redundant tasks by (slot,generation,kind), keeping latest version and urgency.
class WorkBatcher {
public:
    void enqueue(std::span<const GpuWork> jobs);
    // registry, when supplied, is read by the single consumer/owner thread only.
    // Retired handles are removed without ever dispatching stale GPU work.
    std::vector<GpuWork> take(uint32_t maxJobs, uint64_t maxGroups,
                              const ChunkRegistry* registry = nullptr);
    size_t pending() const;
private:
    struct WorkKey {
        ChunkHandle handle;
        WorkKind kind;
        friend bool operator==(const WorkKey&, const WorkKey&) = default;
    };
    struct WorkKeyHash { size_t operator()(const WorkKey&) const noexcept; };
    mutable std::mutex mutex_;
    std::unordered_map<WorkKey, GpuWork, WorkKeyHash> queued_;
};

struct GraphicsDistance {
    uint32_t renderChunks = 48;
    uint32_t actualChunks = 12;
    void normalize() {
        renderChunks = std::clamp(renderChunks, 2u, 48u);
        actualChunks = std::clamp(actualChunks, 2u, renderChunks);
    }
    bool cpuOwned(int dx, int dz) const {
        return int64_t(dx)*dx + int64_t(dz)*dz <= int64_t(actualChunks)*actualChunks;
    }
    bool visible(int dx, int dz) const {
        return int64_t(dx)*dx + int64_t(dz)*dz <= int64_t(renderChunks)*renderChunks;
    }
};

} // namespace OptiCraft::Vulkan
