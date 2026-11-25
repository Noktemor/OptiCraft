#include "rendering/vulkan/GpuWorldCore.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>

namespace OptiCraft::Vulkan {
namespace {
uint64_t mix(uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}
uint64_t aligned(uint64_t n, uint64_t alignment) {
    // Caller guarantees power of two.
    if (n > UINT64_MAX - (alignment - 1)) return UINT64_MAX;
    return (n + alignment - 1) & ~(alignment - 1);
}
}

void packBlockIds(std::span<const uint16_t> voxels, std::span<uint32_t> words) {
    if (voxels.size() != kChunkVoxels || words.size() < kChunkBlockWords)
        throw std::invalid_argument("packBlockIds expects 32768 block IDs and 16384 words");
    for (size_t i = 0; i < kChunkBlockWords; ++i)
        words[i] = uint32_t(voxels[i * 2]) | (uint32_t(voxels[i * 2 + 1]) << 16u);
}

std::vector<uint32_t> packSparseMetadata(std::span<const uint32_t> entries) {
    if(entries.size()%2)throw std::invalid_argument("Malformed sparse chunk metadata");
    if(entries.empty())return {0u};
    std::vector<uint32_t> words(kChunkVoxels/4,0u);
    for(size_t j=0;j<entries.size();j+=2){
        if(entries[j+1]==0)continue;
        const uint32_t i=entries[j]&0x0fffffu;
        if(i>=kChunkVoxels)throw std::out_of_range("Chunk metadata index outside grid");
        const uint32_t shift=(i&3u)*8u;
        auto& word=words[i/4];
        word=(word&~(255u<<shift))|((entries[j+1]&255u)<<shift);
    }
    if(std::all_of(words.begin()+1,words.end(),[&](uint32_t w){return w==words[0];}))
        return {words[0]};
    return words;
}

size_t ChunkKeyHash::operator()(const ChunkKey& k) const noexcept {
    uint64_t h = mix(uint32_t(k.dimension));
    h ^= mix(uint32_t(k.x) + 0x9e3779b97f4a7c15ULL);
    h ^= mix(uint32_t(k.y) + 0x243f6a8885a308d3ULL);
    h ^= mix(uint32_t(k.z) + 0x13198a2e03707344ULL);
    return size_t(h);
}

RangeArena::RangeArena(uint64_t capacity) : capacity_(capacity) {
    if (capacity) free_.emplace(0, capacity);
}

std::optional<BufferRange> RangeArena::allocate(uint64_t bytes, uint64_t alignment) {
    if (!bytes || !alignment || (alignment & (alignment - 1))) return std::nullopt;
    auto chosen = free_.end();
    uint64_t chosenStart = 0, bestWaste = UINT64_MAX;
    for (auto it = free_.begin(); it != free_.end(); ++it) {
        const uint64_t start = aligned(it->first, alignment);
        if (start < it->first || start - it->first > it->second) continue;
        const uint64_t available = it->second - (start - it->first);
        if (available < bytes) continue;
        const uint64_t waste = available - bytes;
        if (waste < bestWaste) { chosen = it; chosenStart = start; bestWaste = waste; }
    }
    if (chosen == free_.end()) return std::nullopt;
    const uint64_t oldStart = chosen->first, oldEnd = oldStart + chosen->second;
    free_.erase(chosen);
    if (chosenStart > oldStart) free_.emplace(oldStart, chosenStart - oldStart);
    const uint64_t end = chosenStart + bytes;
    if (end < oldEnd) free_.emplace(end, oldEnd - end);
    used_ += bytes;
    allocated_.emplace(chosenStart, bytes);
    return BufferRange{chosenStart, bytes};
}

bool RangeArena::owns(BufferRange range) const {
    const auto it = allocated_.find(range.offset);
    return range.size && it != allocated_.end() && it->second == range.size;
}

bool RangeArena::partition(BufferRange slab, std::span<const BufferRange> retained) {
    if (!owns(slab)) return false;
    const uint64_t end=slab.offset+slab.size;
    uint64_t cursor=slab.offset;
    for(const auto range:retained){
        if(!range || range.offset<cursor || range.offset>end || range.size>end-range.offset)return false;
        cursor=range.offset+range.size;
    }
    // Retained and gap allocations are disjoint pieces of one live allocation.
    // Reuse the normal checked release/coalescing path for the unused pieces.
    allocated_.erase(slab.offset);
    cursor=slab.offset;
    for(const auto range:retained){
        if(cursor<range.offset){
            const BufferRange gap{cursor,range.offset-cursor};
            allocated_.emplace(gap.offset,gap.size);
            if(!release(gap))throw std::logic_error("Invalid slab gap");
        }
        allocated_.emplace(range.offset,range.size);
        cursor=range.offset+range.size;
    }
    if(cursor<end){
        const BufferRange gap{cursor,end-cursor};
        allocated_.emplace(gap.offset,gap.size);
        if(!release(gap))throw std::logic_error("Invalid slab tail");
    }
    return true;
}

bool RangeArena::release(BufferRange range) {
    if (!owns(range) || range.offset > capacity_ || range.size > capacity_ - range.offset)
        return false;
    auto after = free_.lower_bound(range.offset);
    if (after != free_.end() && range.size > after->first - range.offset) return false;
    if (after != free_.begin()) {
        auto before = std::prev(after);
        if (before->second > range.offset - before->first) return false;
    }
    const uint64_t originalOffset = range.offset;
    const uint64_t originalSize = range.size;
    const uint64_t end = range.offset + range.size;
    if (after != free_.end() && after->first == end) {
        range.size += after->second;
        free_.erase(after);
    }
    after = free_.lower_bound(range.offset);
    if (after != free_.begin()) {
        auto before = std::prev(after);
        if (before->first + before->second == range.offset) {
            range.offset = before->first;
            range.size += before->second;
            free_.erase(before);
        }
    }
    free_.emplace(range.offset, range.size);
    used_ -= originalSize;
    allocated_.erase(originalOffset);
    return true;
}

ChunkRegistry::ChunkRegistry(uint64_t blockBytes, uint64_t meshBytes)
    : blocks_(blockBytes), meshes_(meshBytes) {}

ChunkHandle ChunkRegistry::lookup(ChunkKey key) const {
    const auto it = index_.find(key);
    return it == index_.end() ? ChunkHandle{} : it->second;
}

ChunkHandle ChunkRegistry::ensure(ChunkKey key, Authority authority) {
    // Changing authority requires completed GPU->CPU readback (finishPromotion).
    // Never silently promote a procedural chunk simply because CPU requested it.
    if (ChunkHandle existing = lookup(key)) return existing;
    uint32_t slot = 0;
    if (!reusableSlots_.empty()) { slot = reusableSlots_.back(); reusableSlots_.pop_back(); }
    else {
        if (slots_.size() > kMaxPackedFaceSlot)
            throw std::overflow_error("GPU chunk slot index exceeds packed face ABI");
        slot = static_cast<uint32_t>(slots_.size()); slots_.emplace_back();
    }
    Slot& s = slots_[slot];
    if (++s.generation == 0) ++s.generation;
    s.live = true;
    s.entry = ChunkEntry{};
    s.entry.key = key;
    s.entry.handle = {slot, s.generation};
    s.entry.authority = authority;
    s.entry.phase = authority == Authority::CpuAuthoritative
        ? ChunkPhase::NeedsCpuUpload : ChunkPhase::NeedsGpuGeneration;
    index_.emplace(key, s.entry.handle);
    return s.entry.handle;
}

ChunkEntry* ChunkRegistry::resolve(ChunkHandle h) {
    if (!h || h.slot >= slots_.size()) return nullptr;
    Slot& s = slots_[h.slot];
    return s.live && s.generation == h.generation ? &s.entry : nullptr;
}
const ChunkEntry* ChunkRegistry::resolve(ChunkHandle h) const {
    return const_cast<ChunkRegistry*>(this)->resolve(h);
}

bool ChunkRegistry::setBlocks(ChunkHandle h, BufferRange bytes, uint64_t readyTimeline) {
    ChunkEntry* e = resolve(h);
    if (!e || !bytes || e->blockData) return false;
    if (!blocks_.owns(bytes)) return false;
    e->blockData = bytes; e->readyTimeline = readyTimeline;
    e->phase = ChunkPhase::Resident;
    return true;
}

bool ChunkRegistry::publishMesh(ChunkHandle h, BufferRange mesh, uint64_t version,
                                uint64_t readyTimeline, uint64_t retireTimeline) {
    ChunkEntry* e = resolve(h);
    if (!e || !mesh || version <= e->publishedVersion || readyTimeline > retireTimeline)
        return false;
    if (!meshes_.owns(mesh) || (e->meshData && mesh.offset == e->meshData.offset)) return false;
    if (e->meshData) pendingRetire_.push_back({retireTimeline, {}, e->meshData, UINT32_MAX});
    e->meshData = mesh;
    e->publishedVersion = version;
    e->readyTimeline = readyTimeline;
    e->phase = ChunkPhase::Resident;
    return true;
}

bool ChunkRegistry::requestPromotion(ChunkHandle h) {
    auto* e = resolve(h);
    if (!e || e->authority != Authority::GpuProcedural || e->phase != ChunkPhase::Resident) return false;
    e->phase = ChunkPhase::PromotionReadback;
    return true;
}
bool ChunkRegistry::finishPromotion(ChunkHandle h) {
    auto* e = resolve(h);
    if (!e || e->phase != ChunkPhase::PromotionReadback) return false;
    e->authority = Authority::CpuAuthoritative;
    e->phase = ChunkPhase::Resident;
    return true;
}
bool ChunkRegistry::requestEviction(ChunkHandle h) {
    auto* e = resolve(h);
    if (!e || e->phase != ChunkPhase::Resident) return false;
    e->phase = ChunkPhase::EvictionReadback;
    return true;
}

bool ChunkRegistry::retire(ChunkHandle h, uint64_t lastUseTimeline) {
    auto* e = resolve(h);
    if (!e) return false;
    pendingRetire_.push_back({std::max(lastUseTimeline, e->readyTimeline),
                              e->blockData, e->meshData, h.slot});
    index_.erase(e->key);
    slots_[h.slot].live = false; // stale handles invalid immediately, slot not yet reusable
    return true;
}
void ChunkRegistry::collect(uint64_t completedTimeline) {
    // Swap-removal: O(N) total, not O(N^2) during mass eviction.
    for (size_t i = 0; i < pendingRetire_.size();) {
        const Deferred retired = pendingRetire_[i];
        if (retired.timeline > completedTimeline) { ++i; continue; }
        if (retired.block) { const bool ok = blocks_.release(retired.block); assert(ok); (void)ok; }
        if (retired.mesh) { const bool ok = meshes_.release(retired.mesh); assert(ok); (void)ok; }
        if (retired.slot != UINT32_MAX) reusableSlots_.push_back(retired.slot);
        pendingRetire_[i] = pendingRetire_.back();
        pendingRetire_.pop_back();
    }
}

size_t WorkBatcher::WorkKeyHash::operator()(const WorkKey& k) const noexcept {
    return size_t(mix((uint64_t(k.handle.generation) << 32) | k.handle.slot)
                  ^ mix(uint64_t(k.kind)));
}
void WorkBatcher::enqueue(std::span<const GpuWork> jobs) {
    if (jobs.empty()) return;
    std::lock_guard lock(mutex_);
    for (const auto& job : jobs) {
        if (!job.handle || !job.estimatedGroups) continue;
        const WorkKey key{job.handle, job.kind};
        const auto [it, inserted] = queued_.try_emplace(key, job);
        if (!inserted) {
            if (job.version >= it->second.version) {
                it->second.version = job.version;
                it->second.estimatedGroups = job.estimatedGroups;
            }
            it->second.priority = std::max(it->second.priority, job.priority);
            it->second.distanceSquared = std::min(it->second.distanceSquared, job.distanceSquared);
        }
    }
}
std::vector<GpuWork> WorkBatcher::take(uint32_t maxJobs, uint64_t maxGroups,
                                       const ChunkRegistry* registry) {
    if (!maxJobs || !maxGroups) return {};
    std::lock_guard lock(mutex_);
    std::vector<GpuWork> order;
    order.reserve(queued_.size());
    for (const auto& [key, job] : queued_) order.push_back(job);
    std::sort(order.begin(), order.end(), [](const GpuWork& a, const GpuWork& b) {
        if (a.priority != b.priority) return a.priority > b.priority;
        if (a.distanceSquared != b.distanceSquared) return a.distanceSquared < b.distanceSquared;
        if (a.handle.slot != b.handle.slot) return a.handle.slot < b.handle.slot;
        return static_cast<int>(a.kind) < static_cast<int>(b.kind);
    });
    std::vector<GpuWork> out;
    out.reserve(std::min<size_t>(maxJobs, order.size()));
    uint64_t total = 0;
    for (const auto& job : order) {
        if (registry && !registry->resolve(job.handle)) {
            queued_.erase(WorkKey{job.handle, job.kind});
            continue;
        }
        if (out.size() == maxJobs) break;
        if (job.estimatedGroups > maxGroups - total) continue;
        out.push_back(job);
        total += job.estimatedGroups;
        queued_.erase(WorkKey{job.handle, job.kind});
    }
    return out;
}
size_t WorkBatcher::pending() const {
    std::lock_guard lock(mutex_);
    return queued_.size();
}
} // namespace OptiCraft::Vulkan
