#include "rendering/vulkan/VulkanChunkStreamPlan.h"
#include "rendering/vulkan/SurfaceLod.h"

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
uint32_t channelWord(const std::vector<uint32_t>& words,size_t index) noexcept {
    return words[words.size()==1?0:index];
}
// Match all source and policy fields, not merely block version. LOD bits
// alone may differ. A delta is never a replacement for a voxel snapshot.
bool sameLodSource(const LiveChunkPacket& a,const LiveChunkPacket& b) noexcept {
    return a.chunkInstance==b.chunkInstance && a.blocksVersion==b.blocksVersion &&
        a.meshVersion==b.meshVersion && a.gpuBlockLightOwned==b.gpuBlockLightOwned &&
        a.lightOwnershipRevision==b.lightOwnershipRevision &&
        (a.meshKinds&~kLodFlags)==(b.meshKinds&~kLodFlags);
}
bool sameLodSource(const LiveChunkPacket& a,const StreamResident& b) noexcept {
    return a.chunkInstance==b.instance && a.blocksVersion==b.blocksVersion &&
        a.meshVersion==b.meshVersion && a.gpuBlockLightOwned==b.gpuBlockLightOwned &&
        a.lightOwnershipRevision==b.lightOwnershipRevision &&
        (a.meshKinds&~kLodFlags)==(b.meshKinds&~kLodFlags);
}
// Light-only updates may change mesh/ownership/LOD revisions, but never
// the original block/meta payload or material classification.
bool sameLightSource(const LiveChunkPacket& a,const LiveChunkPacket& b) noexcept {
    return a.chunkInstance==b.chunkInstance && a.blocksVersion==b.blocksVersion &&
        (a.meshKinds&~kLodFlags)==(b.meshKinds&~kLodFlags);
}
bool sameLightSource(const LiveChunkPacket& a,const StreamResident& b) noexcept {
    return a.chunkInstance==b.instance && a.blocksVersion==b.blocksVersion &&
        (a.meshKinds&~kLodFlags)==(b.meshKinds&~kLodFlags);
}
bool validChannel(const std::vector<uint32_t>& words,size_t denseSize) noexcept {
    return words.size()==1 || words.size()==denseSize;
}
// Exact boundary words: block ID + metadata + packed light, in shader face order.
std::optional<uint32_t> boundaryOf(const LiveChunkPacket& p,std::array<uint32_t,6*32*32>& out){
    if(p.blockWords.size()==1 && p.metaWords.size()==1 && p.lightWords.size()==1){
        const uint32_t b=p.blockWords[0],m=p.metaWords[0],l=p.lightWords[0];
        // Repeated words can encode alternating IDs/bytes; only truly
        // uniform voxels can use one boundary value.
        if((b&65535u)==(b>>16u) && m==(m&255u)*0x01010101u && l==(l&255u)*0x01010101u){
            return (b&65535u)|((m&255u)<<16u)|((l&255u)<<24u);
        }
    }
    for(uint32_t face=0;face<6;++face)for(uint32_t v=0;v<32;++v)for(uint32_t u=0;u<32;++u){
        const uint32_t edge=(face&1u)?31u:0u;
        const uint32_t x=face<2?edge:u;
        const uint32_t y=face<2?v:face<4?edge:v;
        const uint32_t z=face<2?u:face<4?v:edge;
        const uint32_t i=y*1024+z*32+x;
        const uint32_t id=(channelWord(p.blockWords,i/2)>>((i%2)*16))&65535u;
        const uint32_t meta=(channelWord(p.metaWords,i/4)>>((i%4)*8))&255u;
        const uint32_t light=(channelWord(p.lightWords,i/4)>>((i%4)*8))&255u;
        out[face*1024+v*32+u]=id|(meta<<16)|(light<<24);
    }
    if(std::all_of(out.begin()+1,out.end(),[&](uint32_t w){return w==out[0];}))return out[0];
    return std::nullopt;
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
    if(a.lightOwnershipRevision<b.lightOwnershipRevision)return false;
    if (a.blocksVersion != b.blocksVersion) return a.blocksVersion > b.blocksVersion;
    if(a.meshVersion!=b.meshVersion)return a.meshVersion>b.meshVersion;
    if(a.lightOwnershipRevision!=b.lightOwnershipRevision)return a.lightOwnershipRevision>b.lightOwnershipRevision;
    if(a.gpuBlockLightOwned!=b.gpuBlockLightOwned)return false;
    return a.lodRevision>b.lodRevision;
}
bool VulkanChunkStreamPlan::newerThanResident(const LiveChunkPacket& a, const StreamResident& b) noexcept {
    if (a.chunkInstance != b.instance) return a.chunkInstance > b.instance;
    if(a.lightOwnershipRevision<b.lightOwnershipRevision)return false;
    if (a.blocksVersion != b.blocksVersion) return a.blocksVersion > b.blocksVersion;
    if(a.meshVersion!=b.meshVersion)return a.meshVersion>b.meshVersion;
    if(a.lightOwnershipRevision!=b.lightOwnershipRevision)return a.lightOwnershipRevision>b.lightOwnershipRevision;
    if(a.gpuBlockLightOwned!=b.gpuBlockLightOwned)return false;
    return a.lodRevision>b.lodRevision;
}
void VulkanChunkStreamPlan::validate(const LiveChunkPacket& packet) {
    const bool payloadValid=packet.gpuSourceLease
        ? !packet.lodOnly && !packet.lightOnly && packet.blockWords.empty() && packet.metaWords.size()<=1 &&
          validChannel(packet.lightWords,kChunkVoxels/4) && packet.overlayReference.ticket
        : packet.lodOnly
        ? packet.blockWords.empty() && packet.metaWords.empty() && packet.lightWords.empty() && !packet.overlayReference.ticket
        : packet.lightOnly
        ? packet.blockWords.empty() && packet.metaWords.empty() && validChannel(packet.lightWords,kChunkVoxels/4) && !packet.overlayReference.ticket
        : validChannel(packet.blockWords,kChunkBlockWords) && validChannel(packet.metaWords,kChunkVoxels/4) && validChannel(packet.lightWords,kChunkVoxels/4);
    if (!payloadValid || (packet.lodOnly && packet.lightOnly) || packet.chunkInstance == 0)
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
    // Preflight the entire callback before touching the queue. Mixed duplicate
    // keys containing a control packet are ambiguous and must be retried full.
    std::unordered_map<ChunkKey,bool,ChunkKeyHash> seen;
    if(std::any_of(chunks.begin(),chunks.end(),[](const auto& p){return p.lodOnly || p.lightOnly;})) {
        for(const auto& packet:chunks){
            auto [it,inserted]=seen.emplace(packet.position,packet.lodOnly || packet.lightOnly);
            if(!inserted && (it->second || packet.lodOnly || packet.lightOnly))
                throw std::invalid_argument("Vulkan stream: duplicate control key");
            if(!packet.lodOnly && !packet.lightOnly)continue;
            const auto queued=queued_.find(packet.position);
            const auto resident=locations_.find(packet.position);
            const bool matches=queued!=queued_.end()?
                (packet.lightOnly?sameLightSource(packet,queued->second):sameLodSource(packet,queued->second)):
                resident!=locations_.end() && !evictions_.contains(packet.position) &&
                (packet.lightOnly?sameLightSource(packet,slots_[resident->second].data):sameLodSource(packet,slots_[resident->second].data));
            if(!matches)throw LodControlSourceMissing("Vulkan stream: control needs full snapshot");
        }
    }
    std::unordered_set<ChunkKey,ChunkKeyHash> newKeys;
    for (const auto& packet : chunks)
        if (!queued_.contains(packet.position)) newKeys.insert(packet.position);
    if (newKeys.size() > maxQueued_ - std::min<size_t>(queued_.size(), maxQueued_))
        throw std::overflow_error("Vulkan stream backlog full; retry after draining");
    for (auto& packet : chunks) {
        const ChunkKey key = packet.position;
        if(!forcedEvictions_.contains(key))evictions_.erase(key); // ordinary unload may be superseded
        auto found = queued_.find(key);
        if (found == queued_.end()) queued_.emplace(key, std::move(packet));
        else if (newer(packet, found->second)) {
            const bool interactive=packet.interactive || (packet.chunkInstance==found->second.chunkInstance && found->second.interactive);
            packet.interactive=interactive;
            found->second.interactive=interactive;
            if(packet.lodOnly){
                // Keep the authoritative payload (and overlay reference) if it
                // has not reached the GPU yet. Coalesce only camera fields.
                found->second.meshKinds=packet.meshKinds;
                found->second.lodRevision=packet.lodRevision;
                found->second.priority=packet.priority;
            }else if(packet.lightOnly && !found->second.lodOnly && !found->second.lightOnly){
                // Preserve a queued full block/meta payload and snapshot lease.
                auto& full=found->second;
                full.lightWords=std::move(packet.lightWords);
                full.meshVersion=packet.meshVersion;
                full.meshKinds=packet.meshKinds;
                full.lodRevision=packet.lodRevision;
                full.priority=packet.priority;
                full.gpuBlockLightOwned=packet.gpuBlockLightOwned;
                full.lightOwnershipRevision=packet.lightOwnershipRevision;
            }else found->second = std::move(packet);
        }
    }
}
void VulkanChunkStreamPlan::evict(ChunkKey key) {
    std::lock_guard lock(mutex_);
    queued_.erase(key);
    evictions_.insert(key);
}
void VulkanChunkStreamPlan::retireForReplacement(ChunkKey key) {
    std::lock_guard lock(mutex_);
    queued_.erase(key);evictions_.insert(key);forcedEvictions_.insert(key);
}
void VulkanChunkStreamPlan::affected(ChunkKey key, std::unordered_set<uint32_t>& changed, uint32_t sideMask) const {
    for (size_t i=0;i<neighbors.size();++i) {
        if(!(sideMask&(1u<<i)))continue;
        const auto neighbor = adjacent(key, neighbors[i]);
        if (neighbor) {
            auto it=locations_.find(*neighbor);
            if (it != locations_.end()) changed.insert(it->second);
        }
    }
}
StreamBatch VulkanChunkStreamPlan::take(uint32_t maxUploads,const Admission& admit,const ReleaseNotice& onRelease,bool interactiveOnly) {
    StreamBatch batch;
    std::lock_guard lock(mutex_);
    std::unordered_set<uint32_t> dirty,interactiveDirty;
    // Process all unloads before recycling their slots. Render owner must clear
    // GPU draw commands before publishing replacements, with a queue barrier.
    for (ChunkKey key : evictions_) {
        auto it=locations_.find(key);
        if (it == locations_.end()) continue;
        const uint32_t slot=it->second;
        batch.releases.push_back({key,slot,slots_[slot].generation});
        const bool unknownBoundary=!slots_[slot].boundaryKnown;
        slots_[slot].active=false;
        slots_[slot].boundary.reset();
        slots_[slot].boundaryKnown=true;
        locations_.erase(it);
        recycled_.push_back(slot);
        if(onRelease)onRelease(slot);
        affected(key,dirty);
        if(unknownBoundary)for(int x=-1;x<=1;++x)for(int y=-1;y<=1;++y)for(int z=-1;z<=1;++z){
            const auto n=adjacent(key,{x,y,z});
            if(n){auto found=locations_.find(*n);if(found!=locations_.end())dirty.insert(found->second);}
        }
    }
    evictions_.clear();forcedEvictions_.clear();
    // Ordered selection gives deterministic upload ordering (no noisy unordered
    // map iteration) and avoids GPU dispatch when no actual versions changed.
    struct Candidate {ChunkKey key;uint64_t priority;bool resident;bool interactive;};
    std::vector<Candidate> order;
    size_t selected=0;
    auto priorityLess=[](const Candidate& a,const Candidate& b){
        if(a.interactive!=b.interactive)return a.interactive;
        if(a.resident!=b.resident)return a.resident;
        if(a.priority!=b.priority)return a.priority<b.priority;
        if(a.key.dimension!=b.key.dimension)return a.key.dimension<b.key.dimension;
        if(a.key.x!=b.key.x)return a.key.x<b.key.x;
        if(a.key.y!=b.key.y)return a.key.y<b.key.y;
        return a.key.z<b.key.z;
    };
    if(maxUploads){
        order.reserve(queued_.size());
        for(auto it=queued_.begin();it!=queued_.end();){
            const auto resident=locations_.find(it->first);
            if(resident!=locations_.end() && !newerThanResident(it->second,slots_[resident->second].data)){
                it=queued_.erase(it);continue;
            }
            if(!interactiveOnly || it->second.interactive)
                order.push_back({it->first,it->second.priority,resident!=locations_.end(),it->second.interactive});
            ++it;
        }
        // Only the next upload budget needs ordering. Cache comparator fields
        // once rather than doing repeated hash lookups inside an O(N log N) sort.
        selected=std::min<size_t>(maxUploads,order.size());
        std::partial_sort(order.begin(),order.begin()+selected,order.end(),priorityLess);
    }
    for(size_t i=0;i<order.size() && batch.uploads.size()<maxUploads;++i){
        // Only under memory pressure: consider further eligible packets instead
        // of starving uniform uploads behind an unallocatable dense packet.
        if(i==selected)std::sort(order.begin()+i,order.end(),priorityLess);
        const Candidate& candidate=order[i];
        const ChunkKey key=candidate.key;
        auto pending=queued_.find(key);
        auto existing=locations_.find(key);
        const bool newResident = existing == locations_.end();
        uint32_t slot=UINT32_MAX;
        if (!newResident) slot=existing->second;
        else if (!recycled_.empty()) slot=recycled_.back();
        else if (slots_.size() < maxResident_) slot=uint32_t(slots_.size());
        else {if(candidate.interactive)continue;break;} // blocked new chunk must not delay resident edits
        // Inspect the prospective slot without mutating generation/ownership.
        const Slot empty{};
        const Slot& previous=slot<slots_.size()?slots_[slot]:empty;
        if(pending->second.lodOnly){
            // A standalone control packet can only refer to a committed source.
            // Eviction clears queued controls; full snapshots replace them.
            if(newResident || !previous.active || !sameLodSource(pending->second,previous.data))
                throw std::logic_error("Vulkan stream: LOD source disappeared");
            auto& target=slots_[slot];
            target.data.meshKinds=pending->second.meshKinds;
            target.data.lodRevision=pending->second.lodRevision;
            dirty.insert(slot);
            if(candidate.interactive)interactiveDirty.insert(slot);
            batch.uploads.push_back({slot,target.generation,false,false,std::move(pending->second),false});
            queued_.erase(pending);
            continue;
        }
        if(pending->second.lightOnly){
            const auto& p=pending->second;
            if(newResident || !previous.active || !sameLightSource(p,previous.data))
                throw std::logic_error("Vulkan stream: light source disappeared");
            if(admit && !admit(slot,p,true))continue;
            // Reconstruct the exact combined boundary using the previously
            // committed block/meta bytes. No dense block/meta data required.
            std::array<uint32_t,6*32*32> boundary;
            uint32_t sideMask=previous.boundaryKnown?0u:63u;
            if(previous.boundaryKnown)for(uint32_t face=0;face<6;++face)for(uint32_t v=0;v<32;++v)for(uint32_t u=0;u<32;++u){
                const uint32_t edge=(face&1u)?31u:0u;
                const uint32_t x=face<2?edge:u;
                const uint32_t y=face<2?v:face<4?edge:v;
                const uint32_t z=face<2?u:face<4?v:edge;
                const uint32_t i=y*1024+z*32+x, j=face*1024+v*32+u;
                const uint32_t old=previous.boundary?(*previous.boundary)[j]:previous.uniformBoundary;
                const uint32_t light=(channelWord(p.lightWords,i/4)>>((i%4)*8))&255u;
                boundary[j]=(old&0xffffffu)|(light<<24);
                if(boundary[j]!=old)sideMask|=1u<<face;
            }
            const bool owned=previous.data.gpuBlockLightOwned || p.gpuBlockLightOwned;
            auto& target=slots_[slot];
            if(target.boundaryKnown){
                if(std::all_of(boundary.begin()+1,boundary.end(),[&](uint32_t w){return w==boundary[0];})){
                    target.uniformBoundary=boundary[0];target.boundary.reset();
                }else if(target.boundary)*target.boundary=boundary;
                else target.boundary=std::make_unique<std::array<uint32_t,6*32*32>>(boundary);
            }
            target.data.meshVersion=p.meshVersion;
            target.data.meshKinds=p.meshKinds;
            target.data.lodRevision=p.lodRevision;
            target.data.gpuBlockLightOwned=p.gpuBlockLightOwned;
            target.data.lightOwnershipRevision=p.lightOwnershipRevision;
            dirty.insert(slot);affected(key,dirty,owned?63u:sideMask);
            if(owned || !target.boundaryKnown)for(int x=-1;x<=1;++x)for(int y=-1;y<=1;++y)for(int z=-1;z<=1;++z){
                const auto n=adjacent(key,{x,y,z});
                if(n){auto found=locations_.find(*n);if(found!=locations_.end())dirty.insert(found->second);}
            }
            if(candidate.interactive){interactiveDirty.insert(slot);affected(key,interactiveDirty,owned?63u:sideMask);}
            batch.uploads.push_back({slot,target.generation,false,false,std::move(pending->second),true});
            queued_.erase(pending);continue;
        }
        const bool newInstance=!previous.active || previous.data.instance!=pending->second.chunkInstance;
        const bool sourceOnly=bool(pending->second.gpuSourceLease);
        const bool versionUpload=sourceOnly || newInstance || previous.data.blocksVersion!=pending->second.blocksVersion ||
            previous.data.meshVersion!=pending->second.meshVersion ||
            previous.data.gpuBlockLightOwned!=pending->second.gpuBlockLightOwned;
        // Reject known dense uploads before scanning their six voxel borders.
        if(versionUpload && admit && !admit(slot,pending->second,true))continue;
        std::array<uint32_t,6*32*32> boundary;
        bool uniform=false;uint32_t uniformWord=0;
        if(!sourceOnly)if(const auto u=boundaryOf(pending->second,boundary)){uniform=true;uniformWord=*u;}
        uint32_t sideMask=63u;
        if(!sourceOnly && previous.boundaryKnown && !newInstance && (previous.data.meshKinds&~kLodFlags)==(pending->second.meshKinds&~kLodFlags)){
            sideMask=0;
            if(uniform && !previous.boundary)sideMask=uniformWord==previous.uniformBoundary?0u:63u;
            else for(uint32_t face=0;face<6;++face){
                const auto first=face*1024;
                bool equal;
                if(uniform){
                    const auto begin=previous.boundary->begin()+first;
                    equal=std::all_of(begin,begin+1024,[&](uint32_t w){return w==uniformWord;});
                }else if(!previous.boundary){
                    const auto begin=boundary.begin()+first;
                    equal=std::all_of(begin,begin+1024,[&](uint32_t w){return w==previous.uniformBoundary;});
                }else equal=std::equal(boundary.begin()+first,boundary.begin()+first+1024,
                                      previous.boundary->begin()+first);
                if(!equal)sideMask|=1u<<face;
            }
        }
        // A GPU publication may have changed light on any face since the CPU
        // boundary snapshot. Never use that snapshot to suppress invalidation
        // on a voxel upload involving GPU ownership (including handback).
        const bool ownedUpload=(versionUpload || sideMask!=0u) &&
           (previous.data.gpuBlockLightOwned || pending->second.gpuBlockLightOwned);
        if(ownedUpload)sideMask=63u;
        const bool uploadVoxels=versionUpload || sideMask!=0u;
        if(!versionUpload && uploadVoxels && admit && !admit(slot,pending->second,true))continue;
        if(newResident){
            if(!recycled_.empty())recycled_.pop_back();
            else slots_.emplace_back();
        }
        Slot& target=slots_[slot];
        if(newInstance && ++target.generation==0)++target.generation;
        const bool unknownBoundary=sourceOnly || !previous.boundaryKnown;
        target.boundaryKnown=!sourceOnly;
        if(sourceOnly){target.uniformBoundary=0;target.boundary.reset();}
        else if(uniform){target.uniformBoundary=uniformWord;target.boundary.reset();}
        else if(target.boundary)*target.boundary=boundary;
        else target.boundary=std::make_unique<std::array<uint32_t,6*32*32>>(boundary);
        target.active=true;
        target.data={key,slot,target.generation,pending->second.chunkInstance,
                     pending->second.blocksVersion,pending->second.meshVersion,pending->second.meshKinds,pending->second.lodRevision,pending->second.gpuBlockLightOwned,pending->second.lightOwnershipRevision};
        locations_[key]=slot;
        dirty.insert(slot);
        affected(key,dirty,sideMask);
        // GPU-owned light may also have been consumed by diagonal geometry.
        if(ownedUpload || unknownBoundary)for(int x=-1;x<=1;++x)for(int y=-1;y<=1;++y)for(int z=-1;z<=1;++z){
            const auto n=adjacent(key,{x,y,z});
            if(n){auto found=locations_.find(*n);if(found!=locations_.end())dirty.insert(found->second);}
        }
        if(candidate.interactive){
            interactiveDirty.insert(slot);affected(key,interactiveDirty,sideMask);
            if(ownedUpload || unknownBoundary)for(int x=-1;x<=1;++x)for(int y=-1;y<=1;++y)for(int z=-1;z<=1;++z){
                const auto n=adjacent(key,{x,y,z});
                if(n){auto found=locations_.find(*n);if(found!=locations_.end())interactiveDirty.insert(found->second);}
            }
        }
        batch.uploads.push_back({slot,target.generation,newInstance,newResident,std::move(pending->second),uploadVoxels});
        queued_.erase(pending);
    }
    batch.remeshSlots.assign(dirty.begin(),dirty.end());
    std::sort(batch.remeshSlots.begin(),batch.remeshSlots.end());
    batch.interactiveRemeshSlots.assign(interactiveDirty.begin(),interactiveDirty.end());
    std::sort(batch.interactiveRemeshSlots.begin(),batch.interactiveRemeshSlots.end());
    return batch;
}
std::vector<uint32_t> VulkanChunkStreamPlan::liveSlotsSnapshot() const {
    std::lock_guard lock(mutex_);
    std::vector<uint32_t> live;
    live.reserve(locations_.size());
    for(uint32_t i=0;i<slots_.size();++i)if(slots_[i].active)live.push_back(i);
    return live;
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
bool VulkanChunkStreamPlan::fullyOccluded(uint32_t slot) const {
    std::lock_guard lock(mutex_);
    if(slot>=slots_.size() || !slots_[slot].active || (slots_[slot].data.meshKinds&7u)!=5u)return false;
    const auto key=slots_[slot].data.key;
    for(const auto& dir:neighbors){
        const auto neighbor=adjacent(key,dir);
        if(!neighbor)return false;
        const auto it=locations_.find(*neighbor);
        if(it==locations_.end() || (slots_[it->second].data.meshKinds&7u)!=5u)return false;
    }
    return true;
}
size_t VulkanChunkStreamPlan::residentCount() const {
    std::lock_guard lock(mutex_);return locations_.size();
}
size_t VulkanChunkStreamPlan::pendingCount() const {
    std::lock_guard lock(mutex_);return queued_.size()+evictions_.size();
}
bool VulkanChunkStreamPlan::hasPendingChange(ChunkKey key) const {
    std::lock_guard lock(mutex_);
    return queued_.contains(key)||evictions_.contains(key);
}
size_t VulkanChunkStreamPlan::boundaryPayloadBytes() const {
    std::lock_guard lock(mutex_);
    size_t bytes=0;
    for(const auto& slot:slots_)if(slot.active && slot.boundaryKnown)bytes+=slot.boundary?sizeof(*slot.boundary):sizeof(slot.uniformBoundary);
    return bytes;
}
} // namespace OptiCraft::Vulkan
