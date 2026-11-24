#include "rendering/vulkan/VulkanChunkStreamPlan.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace OptiCraft::Vulkan {
namespace {
constexpr std::array<std::array<int, 3>, 6> neighbors{{
    {{-1,0,0}}, {{1,0,0}}, {{0,-1,0}}, {{0,1,0}}, {{0,0,-1}}, {{0,0,1}}
}};
// Avoid signed integer overflow near the edge of an effectively infinite world.
std::optional<ChunkKey> adjacent(ChunkKey k, const std::array<int,3>& d) {
    const int64_t x=int64_t(k.x)+d[0], y=int64_t(k.y)+d[1], z=int64_t(k.z)+d[2];
    if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX ||
        z < INT32_MIN || z > INT32_MAX) return std::nullopt;
    return ChunkKey{k.dimension, int32_t(x),int32_t(y),int32_t(z)};
}
} // namespace

VulkanChunkStreamPlan::VulkanChunkStreamPlan(uint32_t maxResident, uint32_t maxQueued)
    : maxResident_(maxResident), maxQueued_(maxQueued) {
    // GPU slots use 30 bits in PackedFace; shader offsets use signed int32.
    if (!maxResident || maxResident > 131071 || !maxQueued)
        throw std::invalid_argument("Invalid Vulkan chunk stream capacity");
    slots_.reserve(maxResident);
}

bool VulkanChunkStreamPlan::newer(const LiveChunkPacket& a, const LiveChunkPacket& b) noexcept {
    if (a.chunkInstance != b.chunkInstance) return a.chunkInstance > b.chunkInstance;
    if (a.blocksVersion != b.blocksVersion) return a.blocksVersion > b.blocksVersion;
    return a.meshVersion > b.meshVersion;
}
bool VulkanChunkStreamPlan::newerThanResident(const LiveChunkPacket& a, const StreamResident& b) noexcept {
    if (a.chunkInstance != b.instance) return a.chunkInstance > b.instance;
    if (a.blocksVersion != b.blocksVersion) return a.blocksVersion > b.blocksVersion;
    return a.meshVersion > b.meshVersion;
}
void VulkanChunkStreamPlan::validate(const LiveChunkPacket& packet) {
    if (packet.blockWords.size() != kChunkBlockWords ||
        packet.metaWords.size() != kChunkVoxels / 4 ||
        packet.lightWords.size() != kChunkVoxels / 4 ||
        packet.chunkInstance == 0)
        throw std::invalid_argument("Vulkan stream: malformed 32^3 chunk snapshot");
    // Each chunk origin must fit in the GPU shader's signed 32-bit voxel coordinate.
    for (int32_t v : {packet.position.x,packet.position.y,packet.position.z})
        if (int64_t(v) * kChunkSide < INT32_MIN ||
            int64_t(v) * kChunkSide + kChunkSide > INT32_MAX)
            throw std::out_of_range("Vulkan stream: chunk origin outside shader coordinate range");
}

void VulkanChunkStreamPlan::submit(std::vector<LiveChunkPacket>&& chunks) {
    if (chunks.empty()) return;
    // Validation and capacity checks happen BEFORE modifying the queue. Never
    // acknowledge half a callback if it cannot be staged in this bounded pool.
    for (const auto& packet : chunks) validate(packet);
    std::lock_guard lock(mutex_);
    std::unordered_set<ChunkKey,ChunkKeyHash> newKeys;
    for (const auto& packet : chunks)
        if (!queued_.contains(packet.position)) newKeys.insert(packet.position);
    if (newKeys.size() > maxQueued_ - std::min<size_t>(queued_.size(), maxQueued_))
        throw std::overflow_error("Vulkan stream backlog full; retry after draining");
    for (auto& packet : chunks) {
        const ChunkKey key = packet.position;
        evictions_.erase(key); // newer loaded snapshot supersedes pending unload
        auto found = queued_.find(key);
        if (found == queued_.end()) queued_.emplace(key, std::move(packet));
        else if (newer(packet, found->second)) found->second = std::move(packet);
    }
}
void VulkanChunkStreamPlan::evict(ChunkKey key) {
    std::lock_guard lock(mutex_);
    queued_.erase(key);
    evictions_.insert(key);
}
void VulkanChunkStreamPlan::affected(ChunkKey key, std::unordered_set<uint32_t>& changed) const {
    for (const auto& dir : neighbors) {
        const auto neighbor = adjacent(key, dir);
        if (neighbor) {
            auto it=locations_.find(*neighbor);
            if (it != locations_.end()) changed.insert(it->second);
        }
    }
}
StreamBatch VulkanChunkStreamPlan::take(uint32_t maxUploads) {
    StreamBatch batch;
    std::lock_guard lock(mutex_);
    std::unordered_set<uint32_t> dirty;
    // Process all unloads before recycling their slots. Render owner must clear
    // GPU draw commands before publishing replacements, with a queue barrier.
    for (ChunkKey key : evictions_) {
        auto it=locations_.find(key);
        if (it == locations_.end()) continue;
        const uint32_t slot=it->second;
        batch.releases.push_back({key,slot,slots_[slot].generation});
        slots_[slot].active=false;
        locations_.erase(it);
        recycled_.push_back(slot);
        affected(key,dirty);
    }
    evictions_.clear();
    // Ordered selection gives deterministic upload ordering (no noisy unordered
    // map iteration) and avoids GPU dispatch when no actual versions changed.
    std::vector<ChunkKey> order;
    order.reserve(queued_.size());
    for (const auto& [key,packet] : queued_) order.push_back(key);
    std::sort(order.begin(), order.end(), [this](const ChunkKey& a, const ChunkKey& b) {
        // Existing residents must never starve behind uploads of new chunks
        // when every GPU residency slot is already occupied.
        if (locations_.contains(a) != locations_.contains(b))
            return locations_.contains(a);
        if (a.dimension != b.dimension) return a.dimension < b.dimension;
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        return a.z < b.z;
    });
    for (ChunkKey key : order) {
        if (batch.uploads.size() >= maxUploads) break;
        auto pending=queued_.find(key);
        if (pending == queued_.end()) continue;
        auto existing=locations_.find(key);
        if (existing != locations_.end() &&
            !newerThanResident(pending->second,slots_[existing->second].data)) {
            queued_.erase(pending); continue;
        }
        const bool newResident = existing == locations_.end();
        uint32_t slot=UINT32_MAX;
        if (!newResident) slot=existing->second;
        else if (!recycled_.empty()) {slot=recycled_.back();recycled_.pop_back();}
        else if (slots_.size() < maxResident_) {slot=uint32_t(slots_.size());slots_.emplace_back();}
        else break; // keep packets pending; never silently drop world data
        Slot& target=slots_[slot];
        const bool newInstance=!target.active || target.data.instance!=pending->second.chunkInstance;
        if (newInstance && ++target.generation==0) ++target.generation;
        target.active=true;
        target.data={key,slot,target.generation,pending->second.chunkInstance,
                     pending->second.blocksVersion,pending->second.meshVersion};
        locations_[key]=slot;
        dirty.insert(slot);
        affected(key,dirty);
        batch.uploads.push_back({slot,target.generation,newInstance,newResident,std::move(pending->second)});
        queued_.erase(pending);
    }
    batch.remeshSlots.assign(dirty.begin(),dirty.end());
    std::sort(batch.remeshSlots.begin(),batch.remeshSlots.end());
    return batch;
}
std::optional<StreamResident> VulkanChunkStreamPlan::lookup(ChunkKey key) const {
    std::lock_guard lock(mutex_);
    auto it=locations_.find(key);
    if (it==locations_.end()) return std::nullopt;
    return slots_[it->second].data;
}
std::optional<StreamResident> VulkanChunkStreamPlan::slotInfo(uint32_t slot) const {
    std::lock_guard lock(mutex_);
    if (slot>=slots_.size() || !slots_[slot].active) return std::nullopt;
    return slots_[slot].data;
}
size_t VulkanChunkStreamPlan::residentCount() const {
    std::lock_guard lock(mutex_);return locations_.size();
}
size_t VulkanChunkStreamPlan::pendingCount() const {
    std::lock_guard lock(mutex_);return queued_.size()+evictions_.size();
}
} // namespace OptiCraft::Vulkan
