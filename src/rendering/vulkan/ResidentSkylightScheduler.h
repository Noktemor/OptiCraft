#pragma once
#include "rendering/vulkan/VulkanChunkStreamGpu.h"
#include "rendering/vulkan/VulkanResidentLight.h"
#include <algorithm>
#include <iterator>
#include <list>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace OptiCraft::Vulkan {
// Owner-thread interface only. Observe every committed stream batch (including
// empty ones); a missed batch rebuilds the index and dirties all owned cores.
// Call setBoundary with the certified world ceiling, not a loaded Y maximum.
// Reset on world replacement. Only exclusive GPU world owners are scheduled.
class ResidentSkylightScheduler {
public:
    using Boundary=VulkanResidentLight::SkyBoundary;
    static constexpr uint32_t defaultJobBudget=72, defaultProbeBudget=32;
    struct Selection {
        std::vector<ChunkKey> keys;
        std::vector<ChunkHandle> handles;
        uint32_t jobs=0;
        const VulkanChunkStreamGpu* owner=nullptr;
        uint64_t serial=0,revision=0;
    };
    ResidentSkylightScheduler()=default;
    ResidentSkylightScheduler(const ResidentSkylightScheduler&)=delete;
    ResidentSkylightScheduler& operator=(const ResidentSkylightScheduler&)=delete;
    void reset() noexcept {
        clearIndex();boundaries_.clear();owner_=nullptr;serial_=0;++revision_;
    }
    size_t pendingCount() const noexcept {return pending_.size();}
    bool pending(ChunkKey key) const {return pending_.contains(key);}
    // Per-core gate for a future promotion scheduler: the current sky result
    // has been recorded, not necessarily completed by the GPU. Caller must
    // also gate block light and preserve the same-queue/fence ordering.
    bool recordedCurrent(const VulkanChunkStreamGpu& stream,const StreamBatch& batch,ChunkKey key) const {
        const auto state=stream.ownedStateAt(key);
        return owner_==&stream && stream.currentStreamBatch(batch) && batch.streamRecordSerial==serial_ &&
            boundaries_.contains(key.dimension) && tracked(key) && !pending(key) &&
            (state==VulkanChunkStreamGpu::OwnershipState::Resident ||
             state==VulkanChunkStreamGpu::OwnershipState::Promoting);
    }
    // Caller must call invalidateDimension on material transparency changes,
    // even when the ceiling itself is unchanged.
    void setBoundary(int32_t dimension,Boundary boundary) {
        if(!boundary.hasSky)boundary.highestChunkY=0;
        auto it=boundaries_.find(dimension);
        if(it!=boundaries_.end() && it->second.hasSky==boundary.hasSky &&
           it->second.highestChunkY==boundary.highestChunkY)return;
        boundaries_.insert_or_assign(dimension,boundary);
        invalidateDimension(dimension);
    }
    void invalidateDimension(int32_t dimension) {
        ++revision_;
        for(const auto& [column,ys]:columns_)if(column.dimension==dimension)
            for(int32_t y:ys){auto key=column;key.y=y;mark(key);}
    }
    bool observe(const VulkanChunkStreamGpu& stream,const StreamBatch& batch) {
        if(!stream.currentStreamBatch(batch))return false;
        if(owner_==&stream && serial_==batch.streamRecordSerial)return true;
        if(owner_!=&stream || batch.streamRecordSerial!=serial_+1){
            // Boundaries are world-specific: do not carry them across streams.
            if(owner_ && owner_!=&stream)boundaries_.clear();
            clearIndex();++revision_;
            for(uint32_t slot:stream.plan().liveSlotsSnapshot())
                if(auto r=stream.plan().slotInfo(slot))track(stream,r->key);
        }else{
            for(const auto& release:batch.releases)untrack(release.key);
            for(const auto& upload:batch.uploads)if(upload.uploadVoxels)
                track(stream,upload.packet.position);
        }
        owner_=&stream;serial_=batch.streamRecordSerial;
        for(const auto& release:batch.releases)dirtyBelow(release.key);
        for(const auto& upload:batch.uploads)if(upload.uploadVoxels){
            // CPU light-only uploads cannot change sky occlusion. An owned
            // page upload may itself replace the sky nibble, however.
            if(!upload.packet.lightOnly)dirtyBelow(upload.packet.position);
            else if(tracked(upload.packet.position))mark(upload.packet.position);
        }
        return true;
    }
    Selection select(const VulkanChunkStreamGpu& stream,uint32_t jobBudget=defaultJobBudget,
                     uint32_t probeBudget=defaultProbeBudget,std::span<const uint32_t> skyBits={}) {
        Selection out;out.owner=owner_;out.serial=serial_;out.revision=revision_;
        if(owner_!=&stream || !jobBudget || !probeBudget)return out;
        jobBudget=std::min(jobBudget,stream.plan().capacity());
        std::unordered_set<ChunkKey,ChunkKeyHash> halo;
        const size_t probes=std::min<size_t>(probeBudget,queue_.size());
        for(size_t i=0;i<probes;++i){
            const ChunkKey key=queue_.front();
            queue_.splice(queue_.end(),queue_,queue_.begin());
            const auto r=stream.plan().lookup(key);
            const auto state=stream.ownedStateAt(key);
            if(!r || !r->gpuBlockLightOwned || !state){untrack(key);continue;}
            if(*state!=VulkanChunkStreamGpu::OwnershipState::Resident)continue;
            if(!out.keys.empty() && out.keys.front().dimension!=key.dimension)continue;
            auto config=boundaries_.find(key.dimension);
            if(config==boundaries_.end())continue;
            const auto boundary=config->second;
            if(!worldLightBounds.contains(key.y))continue;
            if(!boundary.hasSky){
                if(halo.size()>=jobBudget)continue;
                halo.insert(key);out.keys.push_back(key);out.handles.push_back({r->slot,r->generation});
                continue;
            }
            const int64_t low=std::max<int64_t>(worldLightBounds.low,int64_t(key.y)-1);
            const int64_t high=boundary.highestChunkY;
            // Full in-world halo, then direct-sky chains up to the ceiling
            // or a committed opaque top plane.
            if(high<key.y || high>worldLightBounds.high)continue;
            const int64_t haloTop=std::min<int64_t>(int64_t(key.y)+1,high);
            if((haloTop-low+1)*9>jobBudget)continue;
            std::vector<ChunkKey> additions;
            bool complete=true;
            for(int x=-1;x<=1 && complete;++x)for(int z=-1;z<=1 && complete;++z)
                for(int64_t y=low;y<=high;++y){
                    auto n=key;n.x+=x;n.z+=z;n.y=int32_t(y);
                    if(!halo.contains(n)){
                        if(halo.size()+additions.size()>=jobBudget || !stream.plan().lookup(n)){
                            complete=false;break;
                        }
                        additions.push_back(n);
                    }
                    if(y>=haloTop && stream.skyOccluder(n,skyBits))break;
                }
            if(!complete)continue;
            halo.insert(additions.begin(),additions.end());
            out.keys.push_back(key);out.handles.push_back({r->slot,r->generation});
        }
        out.jobs=uint32_t(halo.size());return out;
    }
    // Only successful publication acknowledges the queue. On an abandoned
    // command buffer caller must reset stream AND scheduler before reuse.
    bool record(VkCommandBuffer cmd,uint32_t frameSlot,VulkanChunkStreamGpu& stream,
                StreamBatch& batch,VulkanResidentLight& runtime,const Selection& selection,
                std::span<const uint32_t> skyBits) {
        if(owner_!=&stream || selection.owner!=&stream || !stream.currentStreamBatch(batch) ||
           batch.streamRecordSerial!=serial_ || selection.serial!=serial_ || selection.revision!=revision_ ||
           selection.keys.empty() || selection.keys.size()!=selection.handles.size() ||
           selection.jobs>runtime.capacity() || runtime.busy())return false;
        const int32_t dimension=selection.keys.front().dimension;
        const auto config=boundaries_.find(dimension);
        if(config==boundaries_.end())return false;
        for(size_t i=0;i<selection.keys.size();++i){
            const auto& key=selection.keys[i];
            const auto r=stream.plan().lookup(key);
            if(key.dimension!=dimension || !pending_.contains(key) || !r || !r->gpuBlockLightOwned ||
               ChunkHandle{r->slot,r->generation}!=selection.handles[i] ||
               stream.ownedStateAt(key)!=VulkanChunkStreamGpu::OwnershipState::Resident)return false;
        }
        if(!runtime.recordStreamSkyCore(cmd,frameSlot,stream,selection.handles,skyBits,config->second) ||
           !runtime.publishStreamCore(cmd,frameSlot,stream,batch))return false;
        for(const auto& key:selection.keys)erase(key);
        return true;
    }
private:
    static ChunkKey columnOf(ChunkKey key) noexcept {key.y=0;return key;}
    void clearIndex() noexcept {pending_.clear();queue_.clear();columns_.clear();}
    bool tracked(ChunkKey key) const {
        auto it=columns_.find(columnOf(key));return it!=columns_.end()&&it->second.contains(key.y);
    }
    void mark(const ChunkKey& key){
        if(pending_.contains(key))return;
        queue_.push_back(key);pending_.emplace(key,std::prev(queue_.end()));
    }
    void erase(const ChunkKey& key){
        auto it=pending_.find(key);if(it==pending_.end())return;
        queue_.erase(it->second);pending_.erase(it);
    }
    void untrack(ChunkKey key){
        auto it=columns_.find(columnOf(key));
        if(it!=columns_.end()){
            it->second.erase(key.y);if(it->second.empty())columns_.erase(it);
        }
        erase(key);
    }
    void track(const VulkanChunkStreamGpu& stream,ChunkKey key){
        const auto r=stream.plan().lookup(key);
        if(!r || !r->gpuBlockLightOwned || !stream.ownedStateAt(key)){untrack(key);return;}
        if(columns_[columnOf(key)].insert(key.y).second)mark(key);
    }
    void dirtyBelow(ChunkKey key){
        // Resident coordinate limits ensure +/-1 is safe. A changed blocker
        // affects every lower core whose vertical halo includes its column,
        // plus the core one level above (lateral light from the lower halo).
        const auto boundary=boundaries_.find(key.dimension);
        if(boundary!=boundaries_.end() && !boundary->second.hasSky){
            // Zero sky is independent of every other page. The core itself
            // may have received new light bytes, so still invalidate it.
            if(tracked(key))mark(key);
            return;
        }
        const int32_t top=key.y+1;
        auto column=columnOf(key);
        for(int x=-1;x<=1;++x)for(int z=-1;z<=1;++z){
            auto c=column;c.x+=x;c.z+=z;
            const auto it=columns_.find(c);if(it==columns_.end())continue;
            for(auto y=it->second.begin();y!=it->second.end() && *y<=top;++y){auto core=c;core.y=*y;mark(core);}
        }
    }
    const VulkanChunkStreamGpu* owner_=nullptr;
    uint64_t serial_=0,revision_=0;
    std::unordered_map<int32_t,Boundary> boundaries_;
    std::unordered_map<ChunkKey,std::set<int32_t>,ChunkKeyHash> columns_;
    std::list<ChunkKey> queue_;
    std::unordered_map<ChunkKey,std::list<ChunkKey>::iterator,ChunkKeyHash> pending_;
};
}
