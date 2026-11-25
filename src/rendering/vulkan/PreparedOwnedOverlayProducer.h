#pragma once
#include "rendering/vulkan/OwnedChunkPromotion.h"
#include "rendering/vulkan/VulkanMaterialWorldgen.h"
#include "world/generation/Gpu_Owned_Worldgen_Input.h"

namespace OptiCraft::Vulkan {
// Owner-thread producer for already prepared exact voxel/structure inputs.
// The manager's callback may enqueue a request, but must not call start()
// itself. The caller supplies exact regional inputs (or resolved base),
// ordered edits and authoritative light for that coordinate. No skylight
// generation or proof of parity is provided by this coordinator.
// Use a dedicated material backend for ONE world generator lifetime; do not
// share its start/poll with other jobs (noise tables and tickets are retained).
// Keep manager, stream and backend alive until cancellation/drain completes.
class PreparedOwnedOverlayProducer {
public:
    enum class Poll { Idle, Pending, Published, Rejected, Failed };
    PreparedOwnedOverlayProducer(Chunk_Manager& manager,VulkanChunkStreamGpu& stream,
                                VulkanMaterialWorldgen& backend)
        :manager_(manager),stream_(stream),backend_(backend){}
    PreparedOwnedOverlayProducer(const PreparedOwnedOverlayProducer&)=delete;
    PreparedOwnedOverlayProducer& operator=(const PreparedOwnedOverlayProducer&)=delete;
    ~PreparedOwnedOverlayProducer(){cancel();}
    bool busy() const noexcept {return job_.has_value();}

    // On busy rejection caller retains the offered request (can retry/cancel).
    // Otherwise this producer consumes the request and either runs it or
    // cancels its pending claim. Never install guessed light or partial terrain.
    bool start(const Chunk_Manager::Gpu_Generation_Request& request,const Gpu_Overlay_Work& work,
               std::vector<uint32_t> light,uint64_t priority=0) {
        if(job_)return false;
        if(!manager_.gpu_chunk_claim_current(request.position,request.claim))return false;
        if(light.size()!=1 && light.size()!=Gpu_Overlay_Work::meta_words){
            manager_.cancel_gpu_chunk_claim(request.position,request.claim);return false;
        }
        job_.emplace(Job{request,std::move(light),priority,false,nullptr,{},{}});
        try {
            if(backend_.start_overlay_snapshot(work))return true;
        } catch(...) {cancel();job_.reset();throw;}
        cancel();job_.reset();return false;
    }
    // One claim = one dispatched level. The caller supplies regional data for
    // precisely that level and exact ordered edits, with no CPU base array.
    // The positive completion certificate is checked before starting Overlay.
    // A refused/incomplete base releases the pending claim for CPU scheduling.
    bool start_certified(const Chunk_Manager::Gpu_Generation_Request& request,
                         const World_Generator& generator,
                         const std::array<Gpu_Material_Column,1024>& columns,
                         const Gpu_Base_Region& region,Gpu_Overlay_Work work,
                         std::vector<uint32_t> light,uint64_t priority=0) {
        if(job_)return false;
        if(!manager_.gpu_chunk_claim_current(request.position,request.claim))return false;
        if(manager_.world_generator()!=&generator || generator.get_dimension_id()!=request.dimension ||
           (certificateWorld_ && certificateWorld_!=&generator) ||
           region.data.size()!=Gpu_Base_Region::words(1) || work.level!=0 ||
           !work.valid_certified_snapshot_input() ||
           (light.size()!=1 && light.size()!=Gpu_Overlay_Work::meta_words)) {
            manager_.cancel_gpu_chunk_claim(request.position,request.claim);return false;
        }
        job_.emplace(Job{request,std::move(light),priority,false,&generator,std::move(work),{}});
        // Even a failed initialization can have cached generator-specific data.
        certificateWorld_=&generator;
        try {
            if(backend_.start_base_certificate(generator,request.position.x,request.position.z,
                                                columns,{request.position.y},region))return true;
        } catch(...) {cancel();job_.reset();throw;}
        cancel();job_.reset();return false;
    }
    // Prepared input is generated for one concrete coordinate and world.
    // Authoritative packed light remains mandatory; no guessed default light.
    bool start_prepared(const Chunk_Manager::Gpu_Generation_Request& request,
                        Gpu_Owned_Worldgen_Input&& input,std::vector<uint32_t> light,
                        uint64_t priority=0) {
        if(job_)return false;
        if(!manager_.gpu_chunk_claim_current(request.position,request.claim))return false;
        const auto* generator=manager_.world_generator();
        if(!generator || !generator->owned_gpu_identity_current(input.identity_) ||
           input.x_!=request.position.x || input.y_!=request.position.y || input.z_!=request.position.z) {
            manager_.cancel_gpu_chunk_claim(request.position,request.claim);return false;
        }
        if(!start_certified(request,*generator,input.columns_,input.region_,std::move(input.overlay_),
                            std::move(light),priority))return false;
        job_->identity=std::move(input.identity_);
        return true;
    }
    // Explicit GPU-light bootstrap. Zero is provisional, not authoritative light.
    // Only call when both resident light schedulers are active for this world,
    // with known sky boundary and a path to populate the required halo. Promotion
    // must use Stage73 readiness checks. This does not register those schedulers.
    bool start_prepared_gpu_lighting(const Chunk_Manager::Gpu_Generation_Request& request,
                                    Gpu_Owned_Worldgen_Input&& input,uint64_t priority=0) {
        if(!start_prepared(request,std::move(input),{0u},priority))return false;
        job_->gpuLighting=true;return true;
    }
    // Cancel only the pending CPU exclusion. GPU work, if submitted, is
    // drained by poll before a new request is accepted. No stale publication.
    void cancel() {
        if(!job_ || job_->canceled)return;
        job_->canceled=true;
        manager_.cancel_gpu_chunk_claim(job_->request.position,job_->request.claim);
    }
    // Output is written only on Published. Caller must retain the returned
    // owned token through promotion or authorized discard and retirement.
    Poll poll(std::optional<ClaimedGpuChunk>& output) {
        if(!job_)return Poll::Idle;
        // Never overwrite a caller-owned published token. Drain after it is retained.
        if(output)return Poll::Pending;
        if(job_->certifiedWork) {
            std::vector<Gpu_Certified_Base> certificates;
            using Status=VulkanMaterialWorldgen::SnapshotPoll;
            const auto status=backend_.poll_base_certificate(certificates);
            if(status==Status::Pending)return Poll::Pending;
            if(status!=Status::Ready){cancel();job_.reset();return Poll::Failed;}
            const auto& req=job_->request;
            // Drain canceled jobs without starting the second GPU pass. A
            // matching new claim at the same coordinate is NOT the old claim.
            if(job_->canceled || manager_.world_generator()!=job_->world ||
               (job_->identity && !job_->world->owned_gpu_identity_current(*job_->identity)) ||
               !manager_.gpu_chunk_claim_current(req.position,req.claim)) {
                cancel();job_.reset();return Poll::Rejected;
            }
            if(certificates.size()!=1 || !certificates[0].ticket || certificates[0].level!=0 ||
               certificates[0].x!=req.position.x || certificates[0].y!=req.position.y ||
               certificates[0].z!=req.position.z) {
                cancel();job_.reset();return Poll::Rejected;
            }
            try {
                if(backend_.start_overlay_certified(*job_->certifiedWork,certificates[0])) {
                    job_->certifiedWork.reset(); // now waiting for Overlay fence
                    return Poll::Pending;
                }
            } catch(...) {cancel();job_.reset();throw;}
            cancel();job_.reset();return Poll::Failed;
        }
        std::shared_ptr<OverlaySnapshot> snapshot;
        const auto status=backend_.poll_overlay_snapshot(snapshot);
        using Status=VulkanMaterialWorldgen::SnapshotPoll;
        if(status==Status::Pending)return Poll::Pending;
        if(status!=Status::Ready || !snapshot || !snapshot->ready){
            cancel();job_.reset();return Poll::Failed;
        }
        auto job=std::move(*job_);job_.reset();
        const auto& req=job.request;
        if(job.canceled || (job.world && manager_.world_generator()!=job.world) ||
           (job.identity && !job.world->owned_gpu_identity_current(*job.identity)) ||
           !manager_.gpu_chunk_claim_current(req.position,req.claim)) {
            manager_.cancel_gpu_chunk_claim(req.position,req.claim);return Poll::Rejected;
        }
        LiveChunkPacket packet;
        packet.position={static_cast<int32_t>(req.dimension),req.position.x,req.position.y,req.position.z};
        packet.chunkInstance=req.claim;packet.blocksVersion=1;packet.meshVersion=1;
        packet.priority=job.priority;
        // Conservative classification: both mesh paths are eligible. No
        // uniform metadata promise or geometry LOD inferred from unknown bytes.
        packet.meshKinds=3;
        packet.lightWords=std::move(job.light);
        packet.gpuBlockLightOwned=job.gpuLighting;
        packet.lightOwnershipRevision=job.gpuLighting?1:0;
        packet.overlayReference={snapshot,snapshot->ticket};
        try {
            auto owned=submitClaimedGpuChunk(manager_,stream_,req.claim,std::move(packet));
            if(owned){output=std::move(owned);return Poll::Published;}
        } catch(...) {
            manager_.cancel_gpu_chunk_claim(req.position,req.claim);
            throw;
        }
        manager_.cancel_gpu_chunk_claim(req.position,req.claim);
        return Poll::Rejected;
    }
private:
    struct Job {
        Chunk_Manager::Gpu_Generation_Request request;
        std::vector<uint32_t> light;
        uint64_t priority;
        bool canceled;
        const World_Generator* world;
        std::optional<Gpu_Overlay_Work> certifiedWork; // engaged while Base is in flight
        std::optional<Gpu_Owned_Worldgen_Identity> identity;
        bool gpuLighting=false;
    };
    Chunk_Manager& manager_;
    VulkanChunkStreamGpu& stream_;
    VulkanMaterialWorldgen& backend_;
    std::optional<Job> job_;
    const World_Generator* certificateWorld_=nullptr;
};
} // namespace OptiCraft::Vulkan
