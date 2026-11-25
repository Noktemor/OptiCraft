#pragma once
#include "rendering/vulkan/OwnedChunkPromotion.h"
#include "rendering/vulkan/VulkanMaterialWorldgen.h"
#include "world/generation/Gpu_Owned_Column_Batch.h"
#include <optional>
#include <algorithm>
#include <utility>
#include <cmath>

namespace OptiCraft::Vulkan {
// Dedicated owner-thread backend, one generator lifetime. No new Base can be
// submitted until all dependent Overlay fences have drained. Only provisional
// GPU lighting mode: caller must run both light schedulers and gate promotion.
// Poll at most once per budgeted step; caller retains published tokens and
// drives the streamer to recycle snapshot leases. No automatic registration.
class OwnedColumnProducer {
public:
    using Request=Chunk_Manager::Gpu_Generation_Request;
    enum class Poll {Idle,Pending,Backpressure,Published,Rejected,Failed,Finished};
    OwnedColumnProducer(Chunk_Manager& manager,VulkanChunkStreamGpu& stream,VulkanMaterialWorldgen& backend)
        :manager_(manager),stream_(stream),backend_(backend){}
    OwnedColumnProducer(const OwnedColumnProducer&)=delete;
    OwnedColumnProducer& operator=(const OwnedColumnProducer&)=delete;
    ~OwnedColumnProducer(){cancel();if(phase_==Phase::Base || phase_==Phase::Overlay)backend_.abandon();}
    bool busy()const noexcept{return phase_!=Phase::Idle;}
    size_t pendingCount()const noexcept{return std::count(pending_.begin(),pending_.end(),true);}

    // Busy: consumes nothing. Otherwise consumes responsibility for every
    // supplied pending claim, including on rejection. Requests must correspond
    // exactly to the batch levels in dispatch order (no foreign ownership).
    bool start(Gpu_Owned_Column_Batch&& batch,std::span<const Request> requests,uint64_t priority=0){
        if(busy())return false;
        const auto* world=manager_.world_generator();
        auto reject=[&]{for(const auto& r:requests)manager_.cancel_gpu_chunk_claim(r.position,r.claim);return false;};
        if(!backend_.enabled() || !world || (world_ && world_!=world) || !world->owned_gpu_identity_current(batch.identity) ||
           requests.empty() || requests.size()>Gpu_Base_Region::levels_reserved || requests.size()!=batch.ys.size() ||
           requests.size()!=batch.overlays.size() || batch.region.data.size()!=Gpu_Base_Region::words(requests.size()))return reject();
        for(size_t i=0;i<requests.size();++i){
            const auto& r=requests[i];
            if(r.dimension!=world->get_dimension_id() || !r.claim || r.generation!=requests[0].generation ||
               r.position.x!=batch.x || r.position.y!=batch.ys[i] || r.position.z!=batch.z ||
               !manager_.gpu_chunk_claim_current(r.position,r.claim) || batch.overlays[i].level!=i ||
               !batch.overlays[i].valid_certified_snapshot_input())return reject();
            for(size_t j=0;j<i;++j)if(requests[j].claim==r.claim || requests[j].position==r.position)return reject();
        }
        for(const auto& c:batch.columns)if(!std::isfinite(c.surface_height) || !(c.padding&Gpu_Base_Region::enabled))return reject();
        batch_.emplace(std::move(batch));count_=requests.size();priority_=priority;cursor_=0;
        for(size_t i=0;i<count_;++i){requests_[i]=requests[i];pending_[i]=true;}
        world_=world;
        try {
            if(backend_.start_base_certificate(*world_,batch_->x,batch_->z,batch_->columns,batch_->ys,batch_->region,batch_->cave.get())){
                phase_=Phase::Base;return true;
            }
        }catch(...){cancel();reset();backend_.abandon();throw;}
        cancel();reset();return false;
    }
    // Adapter for Stage86 Prepared results (or compatible owner-thread records).
    // No claims are transferred on busy; otherwise clears input on success or
    // failure. Copies the regional inputs once into the retained dispatch ABI.
    template<class Prepared>
    bool start_prepared(std::vector<Prepared>& entries,uint64_t priority=0){
        if(busy() || entries.empty())return false;
        auto reject=[&]{for(const auto& e:entries)manager_.cancel_gpu_chunk_claim(e.request.position,e.request.claim);entries.clear();return false;};
        if(entries.size()>Gpu_Base_Region::levels_reserved)return reject();
        try {
            std::vector<Gpu_Owned_Worldgen_Input> inputs;inputs.reserve(entries.size());
            std::array<Request,Gpu_Base_Region::levels_reserved> requests{};
            for(size_t i=0;i<entries.size();++i){if(!entries[i].input)return reject();inputs.push_back(std::move(*entries[i].input));requests[i]=entries[i].request;}
            auto batch=Gpu_Owned_Column_Batch::build(inputs);if(!batch)return reject();
            const bool result=start(std::move(*batch),std::span<const Request>(requests.data(),entries.size()),priority);
            entries.clear();return result;
        }catch(...){reject();throw;}
    }
    // Cancel only unpublished claims. Published tokens remain with caller.
    // Submitted work must still be polled; no new jobs during drain.
    void cancel(){for(size_t i=0;i<count_;++i)release(i);}
    bool cancel(uint64_t claim){for(size_t i=0;i<count_;++i)if(pending_[i]&&requests_[i].claim==claim){release(i);return true;}return false;}
    Poll poll(std::optional<ClaimedGpuChunk>& output,std::array<uint32_t,32>* preview=nullptr){
        if(!busy())return Poll::Idle;
        if(output)return Poll::Backpressure;
        using Status=VulkanMaterialWorldgen::SnapshotPoll;
        try {
            if(phase_==Phase::Base){
                std::vector<Gpu_Certified_Base> certs;
                const auto status=backend_.poll_base_certificate(certs);
                if(status==Status::Pending)return Poll::Pending;
                if(status!=Status::Ready)return fail();
                auto routed=Gpu_Owned_Column_Certificates::route(*batch_,certs);
                if(!routed){cancel();reset();return Poll::Rejected;}
                certificates_=std::move(*routed);phase_=Phase::Select;
            }
            if(phase_==Phase::Overlay){
                std::shared_ptr<OverlaySnapshot> snapshot;
                const auto status=backend_.poll_overlay_snapshot(snapshot);
                if(status==Status::Pending)return Poll::Pending;
                const auto i=cursor_++;phase_=Phase::Select;
                if(status!=Status::Ready)return fail();
                if(!snapshot || !snapshot->ready){release(i);return Poll::Failed;}
                if(!current(i)){release(i);return Poll::Rejected;}
                const auto& req=requests_[i];
                LiveChunkPacket packet;
                packet.position={static_cast<int32_t>(req.dimension),req.position.x,req.position.y,req.position.z};
                packet.chunkInstance=req.claim;packet.blocksVersion=1;packet.meshVersion=1;packet.priority=priority_;
                packet.meshKinds=3;packet.lightWords={0u};packet.gpuBlockLightOwned=true;packet.lightOwnershipRevision=1;
                packet.overlayReference={snapshot,snapshot->ticket};
                auto owned=submitClaimedGpuChunk(manager_,stream_,req.claim,std::move(packet));
                if(!owned){release(i);return Poll::Rejected;}
                if(preview){
                    preview->fill(0);
                    const double top=double(req.position.y)*32.0+32.0;
                    // Procedural surface estimate only. Actual local blockers are scanned on GPU.
                    for(size_t j=0;j<batch_->columns.size();++j)
                        if(batch_->columns[j].surface_height<top)(*preview)[j/32]|=1u<<(j%32);
                }
                pending_[i]=false;output=std::move(owned);return Poll::Published;
            }
            while(cursor_<count_){
                const auto i=cursor_;
                if(!current(i) || !certificates_.levels[i]){release(i);++cursor_;continue;}
                if(!backend_.enabled())return fail();
                if(!backend_.overlaySnapshotAvailable())return Poll::Backpressure;
                if(backend_.start_overlay_certified(batch_->overlays[i],*certificates_.levels[i])){
                    phase_=Phase::Overlay;return Poll::Pending;
                }
                release(i);++cursor_;return Poll::Rejected;
            }
            reset();return Poll::Finished;
        }catch(...){cancel();reset();backend_.abandon();throw;}
    }
private:
    enum class Phase{Idle,Base,Select,Overlay};
    bool current(size_t i)const{
        const auto& r=requests_[i];
        return pending_[i] && manager_.world_generator()==world_ && world_->owned_gpu_identity_current(batch_->identity) &&
            manager_.gpu_chunk_claim_current(r.position,r.claim);
    }
    void release(size_t i){if(pending_[i]){const auto& r=requests_[i];manager_.cancel_gpu_chunk_claim(r.position,r.claim);pending_[i]=false;}}
    void reset(){batch_.reset();certificates_={};count_=cursor_=0;phase_=Phase::Idle;}
    Poll fail(){cancel();reset();backend_.abandon();return Poll::Failed;}
    Chunk_Manager& manager_;VulkanChunkStreamGpu& stream_;VulkanMaterialWorldgen& backend_;
    const World_Generator* world_=nullptr;
    std::optional<Gpu_Owned_Column_Batch> batch_;
    std::array<Request,Gpu_Base_Region::levels_reserved> requests_{};
    std::array<bool,Gpu_Base_Region::levels_reserved> pending_{};
    Gpu_Owned_Column_Certificates certificates_{};
    size_t count_=0,cursor_=0;uint64_t priority_=0;Phase phase_=Phase::Idle;
};
} // namespace OptiCraft::Vulkan
