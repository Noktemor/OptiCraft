#pragma once
#include "rendering/vulkan/VulkanChunkStreamGpu.h"
#include "rendering/vulkan/VulkanResidentLight.h"
#include <algorithm>
#include <iterator>
#include <list>
#include <unordered_map>
#include <unordered_set>

namespace OptiCraft::Vulkan {
// Render-thread only. Observe committed stream batches, never producer requests.
// Dirty keys survive incomplete halo / budget misses. CPU-owned pages are never
// scheduled as core. Work is bounded by halo jobs (not just core count).
class ResidentLightScheduler {
public:
    static constexpr uint32_t defaultJobBudget=36, defaultProbeBudget=32;
    struct Selection {
        std::vector<ChunkKey> keys;
        std::vector<ChunkHandle> handles;
        uint32_t jobs=0;
        uint64_t serial=0;
    };
    ResidentLightScheduler()=default;
    ResidentLightScheduler(const ResidentLightScheduler&)=delete;
    ResidentLightScheduler& operator=(const ResidentLightScheduler&)=delete;
    void reset() noexcept { pending_.clear(); queue_.clear(); owned_.clear(); serial_=0; owner_=nullptr; }
    size_t pendingCount() const noexcept {return pending_.size();}
    bool recordedCurrent(const VulkanChunkStreamGpu& stream,const StreamBatch& batch,ChunkKey key) const {
        return owner_==&stream && stream.currentStreamBatch(batch) && serial_==batch.streamRecordSerial &&
            owned_.contains(key) && !pending_.contains(key);
    }
    void observe(const StreamBatch& batch) {
        const auto* stream=batch.streamOwner;
        if(!stream || !stream->currentStreamBatch(batch))return;
        if(owner_==stream && serial_==batch.streamRecordSerial)return;
        if(owner_!=stream || batch.streamRecordSerial!=serial_+1){
            reset();owner_=stream;serial_=batch.streamRecordSerial;
            for(auto slot:stream->plan().liveSlotsSnapshot())if(auto r=stream->plan().slotInfo(slot);r && r->gpuBlockLightOwned){
                owned_.insert(r->key);queue_.push_back(r->key);pending_.emplace(r->key,std::prev(queue_.end()));
            }
            return;
        }
        serial_=batch.streamRecordSerial;owner_=stream;
        // First establish final ownership; a released slot may be reused by an
        // upload in this same batch. LOD-only packets do not reset voxel light.
        for(const auto& r:batch.releases){owned_.erase(r.key);erase(r.key);}
        for(const auto& u:batch.uploads)if(u.uploadVoxels){
            const auto& k=u.packet.position;
            if(u.packet.gpuBlockLightOwned)owned_.insert(k);
            else {owned_.erase(k);erase(k);}
        }
        if(owned_.empty())return;
        for(const auto& r:batch.releases)dirtyAround(r.key);
        for(const auto& u:batch.uploads)if(u.uploadVoxels){
            // Light-only writes replace the destination's light bytes, not
            // the block IDs or emission sources consumed by its neighbours.
            if(u.packet.lightOnly)mark(u.packet.position);
            else dirtyAround(u.packet.position);
        }
    }
    Selection select(const VulkanChunkStreamPlan& plan,uint32_t jobBudget=defaultJobBudget,
                     uint32_t probeBudget=defaultProbeBudget) {
        Selection out;out.serial=serial_;
        if(jobBudget<18 || !probeBudget)return out;
        std::unordered_set<ChunkKey,ChunkKeyHash> halo;
        const size_t probes=std::min<size_t>(probeBudget,queue_.size());
        for(size_t i=0;i<probes;++i){
            const ChunkKey k=queue_.front();
            // Round-robin: incomplete halos do not starve ready chunks.
            queue_.splice(queue_.end(),queue_,queue_.begin());
            const auto r=plan.lookup(k);
            if(!r || !r->gpuBlockLightOwned){owned_.erase(k);erase(k);continue;}
            if(!worldLightBounds.contains(k.y))continue;
            if(owner_){const auto state=owner_->ownedStateAt(k);
                if(state && *state!=VulkanChunkStreamGpu::OwnershipState::Resident)continue;}
            if(!out.keys.empty() && out.keys.front().dimension!=k.dimension)continue;
            std::vector<ChunkKey> additions;
            bool complete=true;
            for(int x=-1;x<=1;++x)for(int y=-1;y<=1;++y)for(int z=-1;z<=1;++z){
                const auto n=offset(k,x,y,z);
                if(!worldLightBounds.contains(n.y) || halo.contains(n))continue;
                if(!plan.lookup(n)){complete=false;continue;}
                additions.push_back(n);
            }
            if(!complete || halo.size()+additions.size()>jobBudget)continue;
            halo.insert(additions.begin(),additions.end());
            out.keys.push_back(k);out.handles.push_back({r->slot,r->generation});
        }
        out.jobs=uint32_t(halo.size());return out;
    }
    // Only a successful same-frame publication clears dirty keys. A failed
    // record/publication leaves them queued, including a busy frame runtime.
    bool record(VkCommandBuffer cmd,uint32_t frameSlot,VulkanChunkStreamGpu& stream,
                StreamBatch& batch,VulkanResidentLight& runtime,const Selection& selection,
                std::span<const uint32_t> bits,std::span<const uint8_t> emission) {
        if(owner_!=&stream || batch.streamOwner!=&stream ||
           batch.streamRecordSerial!=serial_ || selection.serial!=serial_ ||
           selection.keys.empty() || selection.keys.size()!=selection.handles.size() ||
           selection.jobs>runtime.capacity() || runtime.busy())return false;
        for(size_t i=0;i<selection.keys.size();++i){
            const auto state=stream.ownedStateAt(selection.keys[i]);
            if(state && *state!=VulkanChunkStreamGpu::OwnershipState::Resident)return false;
            const auto r=stream.plan().lookup(selection.keys[i]);
            if(!pending_.contains(selection.keys[i]) || !r || !r->gpuBlockLightOwned ||
               ChunkHandle{r->slot,r->generation}!=selection.handles[i])return false;
        }
        if(!runtime.recordStreamCore(cmd,frameSlot,stream,selection.handles,bits,emission) ||
           !runtime.publishStreamCore(cmd,frameSlot,stream,batch))return false;
        for(const auto& k:selection.keys)erase(k);
        return true;
    }
private:
    // Valid committed coordinates are already constrained to signed voxel
    // origins, so adding/subtracting one chunk coordinate cannot overflow.
    static ChunkKey offset(ChunkKey k,int x,int y,int z) noexcept {k.x+=x;k.y+=y;k.z+=z;return k;}
    void erase(const ChunkKey& k) {
        auto it=pending_.find(k);if(it==pending_.end())return;
        queue_.erase(it->second);pending_.erase(it);
    }
    void mark(const ChunkKey& k){
        if(!owned_.contains(k) || pending_.contains(k))return;
        queue_.push_back(k);pending_.emplace(k,std::prev(queue_.end()));
    }
    void dirtyAround(const ChunkKey& k){
        for(int x=-1;x<=1;++x)for(int y=-1;y<=1;++y)for(int z=-1;z<=1;++z){
            const auto n=offset(k,x,y,z);
            mark(n);
        }
    }
    uint64_t serial_=0;
    const VulkanChunkStreamGpu* owner_=nullptr;
    std::unordered_set<ChunkKey,ChunkKeyHash> owned_;
    std::list<ChunkKey> queue_;
    std::unordered_map<ChunkKey,std::list<ChunkKey>::iterator,ChunkKeyHash> pending_;
};
}
