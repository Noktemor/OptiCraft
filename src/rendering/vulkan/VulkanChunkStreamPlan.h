#pragma once

#include "rendering/vulkan/VulkanGameChunkFeed.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OptiCraft::Vulkan {

// CPU *scheduling* only. The submitted arrays contain original blocks/meta/light;
// not a single face or triangle is constructed on the CPU. Game may submit from
// another thread while the Vulkan owner drains a bounded batch once per frame.
struct StreamUpload {
    uint32_t slot = UINT32_MAX;
    uint32_t generation = 0;
    bool resetDraw = false; // new slot or new chunk instance: old geometry is invalid
    bool newResident = false; // only first residency changes the dense live-slot list
    LiveChunkPacket packet;
};
struct StreamRelease {
    ChunkKey key{};
    uint32_t slot = UINT32_MAX;
    uint32_t generation = 0;
};
struct StreamBatch {
    std::vector<StreamUpload> uploads;
    std::vector<StreamRelease> releases;
    // These slots changed themselves OR lost/gained a neighbor; GPU must remesh.
    std::vector<uint32_t> remeshSlots;
    [[nodiscard]] bool empty() const noexcept {
        return uploads.empty() && releases.empty() && remeshSlots.empty();
    }
};
struct StreamResident {
    ChunkKey key{};
    uint32_t slot = UINT32_MAX;
    uint32_t generation = 0;
    uint64_t instance = 0;
    uint64_t blocksVersion = 0;
    uint64_t meshVersion = 0;
};

class VulkanChunkStreamPlan {
public:
    explicit VulkanChunkStreamPlan(uint32_t maxResident = 1024,
                                   uint32_t maxQueued = 2048);
    VulkanChunkStreamPlan(const VulkanChunkStreamPlan&) = delete;
    VulkanChunkStreamPlan& operator=(const VulkanChunkStreamPlan&) = delete;

    // Transactionally reserve queue capacity before consuming the caller's batch.
    // Throws on malformed packets or saturated queue; caller retries after drain.
    void submit(std::vector<LiveChunkPacket>&& chunks);
    void evict(ChunkKey key);
    // One render/stream owner thread only. Other threads may call submit/evict.
    StreamBatch take(uint32_t maxUploads);
    [[nodiscard]] std::optional<StreamResident> lookup(ChunkKey key) const;
    [[nodiscard]] std::optional<StreamResident> slotInfo(uint32_t slot) const;
    [[nodiscard]] uint32_t capacity() const noexcept { return maxResident_; }
    [[nodiscard]] size_t residentCount() const;
    [[nodiscard]] size_t pendingCount() const;
private:
    struct Slot {
        StreamResident data{};
        bool active = false;
        uint32_t generation = 0;
    };
    static bool newer(const LiveChunkPacket& a, const LiveChunkPacket& b) noexcept;
    static bool newerThanResident(const LiveChunkPacket& a, const StreamResident& b) noexcept;
    static void validate(const LiveChunkPacket& packet);
    void affected(ChunkKey key, std::unordered_set<uint32_t>& changed) const;

    const uint32_t maxResident_;
    const uint32_t maxQueued_;
    mutable std::mutex mutex_;
    std::unordered_map<ChunkKey, LiveChunkPacket, ChunkKeyHash> queued_;
    std::unordered_set<ChunkKey, ChunkKeyHash> evictions_;
    std::unordered_map<ChunkKey, uint32_t, ChunkKeyHash> locations_;
    std::vector<Slot> slots_;
    std::vector<uint32_t> recycled_;
};

} // namespace OptiCraft::Vulkan
