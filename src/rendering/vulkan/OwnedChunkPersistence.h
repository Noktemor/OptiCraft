#pragma once
#include "rendering/vulkan/OwnedChunkPromotion.h"
#include "rendering/vulkan/BoundedPersistenceWorker.h"
#include "world/blocks/Block_Types.h"
#include <array>
#include <span>
#include <algorithm>

namespace OptiCraft::Vulkan {
// Region format persists block IDs and metadata; lighting is reconstructed on
// load. The immutable block registry must outlive the writer.
struct OwnedPersistenceJob {
    std::shared_ptr<Region_File> region;
    VulkanChunkStreamGpu::ResidentReadback snapshot;
    static bool write(const OwnedPersistenceJob& job){
        constexpr size_t n=Chunk_Block_Grid::voxel_count;
        const auto& b=job.snapshot.words[0];const auto& m=job.snapshot.words[1];
        if(!job.region||(b.size()!=1&&b.size()!=n/2)||(m.size()!=1&&m.size()!=n/4))return false;
        std::array<Block_ID,n> flat{};
        for(size_t i=0;i<n;++i)flat[i]=Block_ID(b[b.size()==1?0:i/2]>>((i&1u)*16u));
        Chunk_Block_Grid grid;grid.assign_flat(flat.data());
        const auto& registry=Block_Registry::get_instance();
        for(auto id:grid.palette())if(id!=Block_ID(Block_Types::Air)&&!registry.is_known(id))return false;
        for(size_t i=0;i<n;++i){const auto v=uint8_t(m[m.size()==1?0:i/4]>>((i&3u)*8u));if(v)grid.set_meta_by_index(uint32_t(i),v);}
        const auto k=job.snapshot.source.key;
        return job.region->write({k.x,k.y,k.z},grid);
    }
};
// Owner thread, after stream.record + light publication, before render pass.
// Persistence and promotion share exclusive stream tokens and readback slots.
// Once a disk write is accepted, never cancel even on camera movement: a
// partial write may already be visible to storage. Retry the pinned snapshot.
// Caller must stop admission and pump to zero before replacing manager/world.
class OwnedChunkPersistence {
    using Stream=VulkanChunkStreamGpu;
    using Worker=BoundedPersistenceWorker<OwnedPersistenceJob,Stream::kFrameSlots>;
    enum class Phase {Reading,CancelOnly,Writing,Retry,Retiring,Written};
    struct Attempt {
        ClaimedGpuChunk chunk;Stream::PromotionToken token;
        std::shared_ptr<Region_File> region;
        Phase phase=Phase::Reading;
        std::shared_ptr<const OwnedPersistenceJob> job{};
        unsigned retry=0;
    };
public:
    struct Result {bool accepted=false,recorded=false,blocked=false;unsigned saved=0,failed=0;
        std::array<std::optional<ClaimedGpuChunk>,Stream::kFrameSlots> released{};};
    OwnedChunkPersistence(Chunk_Manager& manager,Stream& stream):manager_(manager),stream_(stream),worker_(&OwnedPersistenceJob::write){}
    size_t activeCount() const noexcept {size_t n=0;for(auto& a:attempts_)n+=bool(a);return n;}
    // Candidate span is filtered by the residency policy. Only finalized sources
    // should be supplied. No discard based on unverified procedural parity.
    Result pump(VkCommandBuffer cmd,uint32_t slot,const StreamBatch& batch,
                std::span<const ClaimedGpuChunk> candidates,bool admit=true){
        Result out;
        if(slot>=attempts_.size()||!stream_.currentStreamFrame(cmd,slot,batch)||lastSerial_==batch.streamRecordSerial)return out;
        lastSerial_=batch.streamRecordSerial;out.accepted=true;
        for(size_t i=0;i<attempts_.size();++i){
            auto& a=attempts_[i];if(!a)continue;
            if(a->phase==Phase::Retiring){
                if(!stream_.ownershipState(a->chunk.owner())){
                    if(releaseRetiredGpuChunk(manager_,stream_,a->chunk)){out.released[i]=a->chunk;a.reset();}
                    else out.blocked=true;
                }continue;
            }
            if(a->phase==Phase::CancelOnly){if(stream_.cancelOwnedPromotion(a->token))a.reset();else out.blocked=true;continue;}
            if(a->phase==Phase::Reading){
                if(i!=slot)continue;
                auto snap=stream_.takeOwnedReadback(slot,a->token);
                if(!snap){out.blocked=true;continue;}
                if(!claimedChunkCurrent(manager_,a->chunk)||!stream_.residentReadbackQuiescent(*snap)){
                    a->phase=Phase::CancelOnly;if(stream_.cancelOwnedPromotion(a->token))a.reset();else out.blocked=true;continue;
                }
                try {a->job=std::make_shared<OwnedPersistenceJob>(OwnedPersistenceJob{a->region,std::move(*snap)});}
                catch(...) {a->phase=Phase::CancelOnly;if(stream_.cancelOwnedPromotion(a->token))a.reset();else out.blocked=true;continue;}
                a->phase=Phase::Retry;
            }
            if(a->phase==Phase::Writing){
                const auto state=worker_.poll(i);
                if(state==Worker::State::Failed){a->phase=Phase::Retry;a->retry=30;++out.failed;}
                else if(state==Worker::State::Success)a->phase=Phase::Written;
            }
            if(a->phase==Phase::Retry){
                if(a->retry){--a->retry;continue;}
                if(!claimedChunkCurrent(manager_,a->chunk)||!stream_.residentReadbackQuiescent(a->job->snapshot)){out.blocked=true;continue;}
                if(worker_.submit(i,a->job))a->phase=Phase::Writing;else out.blocked=true;
            }
            if(a->phase==Phase::Written){
                // This callback does not create a CPU chunk. The normal loader
                // can read the saved region after acknowledged retirement.
                if(claimedChunkCurrent(manager_,a->chunk)&&stream_.commitOwnedPromotion(a->token,a->job->snapshot,[]{return true;})){
                    a->phase=Phase::Retiring;a->job.reset();++out.saved;
                }else out.blocked=true; // never release a stale/foreign claim
            }
        }
        if(!admit||attempts_[slot]||out.blocked||candidates.empty())return out;
        const size_t count=std::min(size_t(64),candidates.size());
        for(size_t i=0;i<count;++i){
            const auto& c=candidates[(cursor_+i)%candidates.size()];const auto k=c.owner().key;
            if(c.owner().owner!=&stream_||k.dimension<0||k.dimension>UINT16_MAX||!claimedChunkCurrent(manager_,c)||
                stream_.ownershipState(c.owner())!=Stream::OwnershipState::Resident)continue;
            auto region=manager_.gpu_chunk_persistence_region(claimedPosition(c),static_cast<Dimension_Id>(k.dimension),c.claim());
            if(!region)continue;
            // Claim and CPU promotion tickets share the manager's serial domain.
            // No CPU reservation is needed for an out-of-window save.
            auto token=stream_.beginOwnedPromotion(c.owner(),c.claim());if(!token)continue;
            attempts_[slot].emplace(Attempt{c,*token,std::move(region)});
            if(stream_.recordOwnedReadback(cmd,slot,*token)){cursor_=(cursor_+i+1)%candidates.size();out.recorded=true;return out;}
            auto& a=attempts_[slot];a->phase=Phase::CancelOnly;
            if(stream_.cancelOwnedPromotion(*token))a.reset();else out.blocked=true;
            break;
        }cursor_=(cursor_+count)%candidates.size();return out;
    }
private:
    Chunk_Manager& manager_;Stream& stream_;Worker worker_;
    std::array<std::optional<Attempt>,Stream::kFrameSlots> attempts_{};uint64_t lastSerial_=0;size_t cursor_=0;
};
}
