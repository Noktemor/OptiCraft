#pragma once

#include "rendering/vulkan/GpuWorldCore.h"
#include <cstdint>
#include <array>
#include <chrono>
#include <functional>
#include <unordered_map>
#include <vector>

class Chunk_Manager;
struct Renderable_Chunk;

namespace OptiCraft::Vulkan {

// Persistent original voxel state from the real game, not a synthetic scene.
// The feed emits only new/modified resident chunks, in ONE batched callback.
// It neither creates CPU meshes nor performs Vulkan calls. Consumers may move
// packets into their staging queue before the callback returns.
struct LiveChunkPacket {
    ChunkKey position{};
    uint64_t chunkInstance = 0;
    uint64_t blocksVersion = 0;
    uint32_t meshKinds = 3; // bit 0 opaque, bit 1 misc, bit 2 all cells opaque; conservative default
    uint64_t priority = 0; // camera distance squared at submission
    uint64_t meshVersion = 0; // source dirty epoch, independent of GL mesh publication
    // Each channel is either the full array or one repeated packed uint32_t.
    // A single word is lossless, including alternating IDs/byte patterns.
    std::vector<uint32_t> blockWords; // 16 bits per voxel, 16384 words or 1
    std::vector<uint32_t> metaWords;  // 8 bits per voxel, 8192 words or 1
    std::vector<uint32_t> lightWords; // 8 bits per voxel, 8192 words or 1
    uint64_t lodRevision=0; // monotonic camera-driven topology revision
};

class VulkanGameChunkFeed {
public:
    using UploadSink = std::function<void(std::vector<LiveChunkPacket>&&)>;
    using EvictSink = std::function<void(ChunkKey)>;
    void setSink(UploadSink upload, EvictSink evict = {});
    // Keep GPU residency bounded and prioritize the chunks nearest the
    // actual camera. No GLM dependency in the GPU-world scheduling core.
    void setPriorityCenter(std::array<int32_t,3> chunk) noexcept { centerChunk_=chunk; }
    void setTrackedBudget(size_t count) noexcept { trackedBudget_=count; }
    void setMeshKinds(const std::array<uint8_t,65536>& kinds) { meshKinds_=kinds; hasKinds_=true; }
    void acceptFrame(const std::vector<Renderable_Chunk>& rendered, const Chunk_Manager& world,
                     int32_t dimension = 0, size_t maxPackets = 16);
    void chunkUnloaded(ChunkKey key);
    void clear(); // call on reload; old chunks must not survive a dimension change
    bool active() const { return static_cast<bool>(upload_); }
    size_t trackedCount() const { return sent_.size(); }
    size_t selectedCount() const { return selectedKeys_.size(); }
    size_t remoteAirSkipped() const { return remoteAirSkipped_; }
private:
    struct LastVersion {uint64_t instance=0, blocks=0, mesh=0;uint32_t lod=0;bool opaque=false,geometryEligible=false;};
    uint64_t nextLodRevision_=0;
    std::unordered_map<ChunkKey, LastVersion, ChunkKeyHash> sent_;
    size_t trackedBudget_=512,remoteAirSkipped_=0;
    std::array<uint8_t,65536> meshKinds_{};
    bool hasKinds_=false;
    std::array<int32_t,3> centerChunk_{}, selectionCenter_{};
    std::vector<ChunkKey> selectedKeys_;
    uint64_t selectionEpoch_=UINT64_MAX;
    uint64_t selectionDirtyEpoch_=UINT64_MAX,snapshotDirtyEpoch_=UINT64_MAX;
    int32_t selectionDimension_=0;
    size_t selectionBudget_=0;
    std::chrono::steady_clock::time_point lastSelection_{},lastSnapshot_{},lastCompleteScan_{};
    UploadSink upload_;
    EvictSink evict_;
};
} // namespace OptiCraft::Vulkan
