#pragma once
#include "rendering/vulkan/OwnedColumnProducer.h"
#include "rendering/vulkan/OwnedChunkPersistence.h"
#include "rendering/vulkan/OwnedChunkPromotionScheduler.h"
#include "rendering/vulkan/ResidentSkylightScheduler.h"
#include "world/generation/OwnedWorldgenPreparationQueue.h"
#include <algorithm>
#include "rendering/vulkan/WorldResidencyBudget.h"
#include "rendering/vulkan/AdaptiveWorldWorkBudget.h"
#include "rendering/vulkan/DeviceMemoryUsage.h"
#include "rendering/vulkan/PreviewLightInvalidation.h"

namespace OptiCraft::Vulkan {
// One owner-thread coordinator per world. Bounded experimental admission;
// workers and GPU resources are quiesced before the manager is destroyed.
class OwnedWorldRuntime {
    struct Entry { ClaimedGpuChunk chunk; std::array<uint32_t,32> top; bool previewed=false,blockPreviewed=false; unsigned queuedFrames=0; };
public:
    // Reserve a bounded fraction of the shared table; no slot migration.
    static constexpr uint32_t reservedCapacity(uint32_t total) noexcept {
        return total>=64?std::clamp(total/8u,32u,256u):0u;
    }
    static constexpr uint64_t denseBytesPerChunk=131072;
    static WorldResidencyBudget::Limits limits(uint32_t reserve,uint64_t bytes) noexcept {
        return {reserve, reserve, std::min(8u,reserve/4u),bytes};
    }
    OwnedWorldRuntime(Chunk_Manager& manager,VulkanChunkStreamGpu& stream,VulkanFrameRuntime& runtime,
                      uint32_t reserve=32,uint64_t voxelBudget=256ull<<20)
      :manager_(manager),stream_(stream),runtime_(runtime),dimension_(int32_t(manager.world_generator()->get_dimension_id())),
       hasSky_(manager.world_generator()->get_dimension().has_sky_light),backend_(runtime),
       prep_(manager,*manager.world_generator()),producer_(manager,stream,backend_),promotion_(manager,stream),
       persistence_(std::make_unique<OwnedChunkPersistence>(manager,stream)),
       capacity_(reserve),residency_(limits(reserve,voxelBudget)) {
        entries_.reserve(capacity_); candidates_.reserve(capacity_);outside_.reserve(capacity_);previewSeeds_.reserve(capacity_);
        manager_.set_gpu_generation_producer(6,[this](const auto& r){
            if(!accepting_)return false;
            const auto pending=preparationCount()+producer_.pendingCount();
            // Count both unbound work and bound queued/resident/retiring entries.
            const WorldResidencyBudget::Usage usage{uint32_t(entries_.size()),uint32_t(pending),
                stream_.voxelAllocatedBytes(),pending*denseBytesPerChunk};
            const auto decision=residency_.update(usage,manager_.gpu_world_window(),1,denseBytesPerChunk);
            const bool memoryAllowed=memoryAdmission_.allow(runtime_.deviceMemoryUsage(),pending*denseBytesPerChunk,denseBytesPerChunk);
            pressure_=decision.pressure||!memoryAllowed;
            return memoryAllowed && decision.admissions && prep_.try_enqueue(r);
        });
    }
    ~OwnedWorldRuntime(){ if(!closed_)closeIdle(); }
    size_t preparationCount()const {
        const auto s=prep_.stats();return s.queued+s.running+s.ready+s.failed+s.draining;
    }
    size_t generationPending()const {return preparationCount()+producer_.pendingCount();}
    uint32_t capacity()const noexcept{return capacity_;}
    bool pressure()const noexcept{return pressure_;}
    bool needsTransfers()const {
        const auto w=manager_.gpu_world_window();
        return std::any_of(entries_.begin(),entries_.end(),[&](const Entry& e){
            const auto k=e.chunk.owner().key;
            return !Gpu_World_Streaming::candidate(w,k.x,k.y,k.z);
        });
    }
    void pumpPreparation(bool allowStart=true){
        prep_.prune_stale();
        if(producer_.busy()){
            std::optional<ClaimedGpuChunk> result;std::array<uint32_t,32> top{};
            const auto status=producer_.poll(result,&top);
            if(result){if(!hasSky_)top.fill(0);entries_.push_back({*result,top});}
            if(status==OwnedColumnProducer::Poll::Failed)stopAdmission();
        }else if(accepting_ && allowStart){
            std::vector<OwnedWorldgenPreparationQueue::Prepared> work;
            if(prep_.poll_column(work)==OwnedWorldgenPreparationQueue::Poll::Ready &&
               !producer_.start_prepared(work) && !backend_.enabled())stopAdmission();
        }
    }
    template<class BlockScheduler>
    void recordBlockPreview(VkCommandBuffer cmd,uint32_t slot,StreamBatch& batch,VulkanResidentLight& light,
            const BlockScheduler& block,std::span<const uint32_t> bits,std::span<const uint8_t> emission){
        if(light.busy())return;
        for(size_t i=0,n=entries_.size();i<n;++i){
            auto& e=entries_[blockCursor_++%n];
            const auto key=e.chunk.owner().key;
            if(e.blockPreviewed || stream_.ownershipState(e.chunk.owner())!=VulkanChunkStreamGpu::OwnershipState::Resident)continue;
            if(block.recordedCurrent(stream_,batch,key)){e.blockPreviewed=true;continue;}
            auto r=stream_.plan().lookup(key);if(!r)continue;
            if(light.recordStreamPreviewBlock(cmd,slot,stream_,{r->slot,r->generation},bits,emission,true) &&
               light.publishStreamCore(cmd,slot,stream_,batch))e.blockPreviewed=true;
            if(light.busy())break;
        }
    }
    void observe(const StreamBatch& batch){
        sky_.observe(stream_,batch);sky_.setBoundary(dimension_,{hasSky_,Config::world_max_chunk_y});
        // Exact and provisional tracking are independent. Recompute previews
        // when a neighbour arrives/leaves or changes geometry, not when light
        // itself is published. A missed committed batch invalidates all.
        if(previewSerial_ && batch.streamRecordSerial==previewSerial_)return;
        const bool missed=previewSerial_ && batch.streamRecordSerial!=previewSerial_+1;
        for(auto& e:entries_){
            const auto core=e.chunk.owner().key;
            const auto affected=[&](auto predicate){
                return missed || std::any_of(batch.releases.begin(),batch.releases.end(),[&](const auto& r){
                    return predicate(core,r.key);}) ||
                    std::any_of(batch.uploads.begin(),batch.uploads.end(),[&](const auto& u){
                    return u.uploadVoxels && !u.packet.lightOnly && predicate(core,u.packet.position);});
            };
            if(affected(previewLightAffected<decltype(e.chunk.owner().key)>))e.blockPreviewed=false;
            if(hasSky_?affected(previewSkyAffected<decltype(e.chunk.owner().key)>):affected(previewLightAffected<decltype(e.chunk.owner().key)>))e.previewed=false;
        }
        previewSerial_=batch.streamRecordSerial;
    }
    void recordSky(VkCommandBuffer cmd,uint32_t slot,StreamBatch& batch,VulkanResidentLight& light,
                   std::span<const uint32_t> transparency){
        auto selection=sky_.select(stream_,light.capacity(),ResidentSkylightScheduler::defaultProbeBudget,transparency);
        if(!selection.keys.empty()&&sky_.record(cmd,slot,stream_,batch,light,selection,transparency))return;
        if(light.busy() || !hasSky_)return;
        // No-sky dimensions use the exact core-only scheduler exclusively.
        // Render-only provisional sky. Never acknowledge the exact scheduler.
        // Snapshot the known procedural masks of currently resident owners.
        previewSeeds_.clear();
        for(const auto& e:entries_){
            if(stream_.ownershipState(e.chunk.owner())!=VulkanChunkStreamGpu::OwnershipState::Resident)continue;
            if(auto r=stream_.plan().lookup(e.chunk.owner().key))previewSeeds_.push_back({{r->slot,r->generation},e.top});
        }
        for(size_t i=0,n=entries_.size();i<n;++i){
            auto& e=entries_[skyCursor_++%n];
            auto key=e.chunk.owner().key;
            if(e.previewed || stream_.ownershipState(e.chunk.owner())!=VulkanChunkStreamGpu::OwnershipState::Resident)continue;
            if(sky_.recordedCurrent(stream_,batch,key)){e.previewed=true;continue;}
            auto r=stream_.plan().lookup(key);if(!r)continue;
            if(light.recordStreamPreviewSky(cmd,slot,stream_,{r->slot,r->generation},transparency,e.top,previewSeeds_,true,
                    hasSky_?std::optional<int32_t>{Config::world_max_chunk_y}:std::nullopt) &&
               light.publishStreamCore(cmd,slot,stream_,batch))e.previewed=true;
            if(light.busy())break;
        }
    }
    template<class BlockScheduler>
    bool pumpOwnership(VkCommandBuffer cmd,uint32_t slot,const StreamBatch& batch,const BlockScheduler& block,bool allowNew=true){
        const auto window=manager_.gpu_world_window();
        candidates_.clear();outside_.clear();
        for(auto& e:entries_){
            const auto state=stream_.ownershipState(e.chunk.owner());
            if(!state)continue;
            const auto key=e.chunk.owner().key;
            const bool near=OwnedChunkPromotionScheduler::wanted(window,dimension_,key);
            const bool far=Gpu_World_Streaming::candidate(window,key.x,key.y,key.z);
            if(*state==VulkanChunkStreamGpu::OwnershipState::Queued){
                // No slot was published; avoid pinning a never-admitted lease forever.
                if(!far || ++e.queuedFrames>120)discardClaimedGpuChunk(manager_,stream_,e.chunk);
            }
            if(near)candidates_.push_back(e.chunk);
            else if(!far)outside_.push_back(e.chunk);
        }
        auto p=promotion_.pump(cmd,slot,batch,window,dimension_,candidates_,block,sky_,allowNew,true);
        const auto saved=persistence_->pump(cmd,slot,batch,outside_,allowNew&&!p.recorded);
        // The schedulers acknowledge committed retirement themselves. Also
        // acknowledge explicit discards of never-admitted procedural sources.
        std::erase_if(entries_,[&](const Entry& e){
            if(!claimedChunkCurrent(manager_,e.chunk))return true;
            return !stream_.ownershipState(e.chunk.owner()) && releaseRetiredGpuChunk(manager_,stream_,e.chunk);
        });
        return p.recorded || saved.recorded;
    }
    void closeIdle(){
        if(closed_)return;
        stopAdmission();prep_.shutdown();producer_.cancel();promotion_.cancelIdle();
        // Join accepted disk writes before removing any world/storage references.
        persistence_.reset();stream_.retireOwnedIdle();
        for(auto& e:entries_)releaseRetiredGpuChunk(manager_,stream_,e.chunk);
        entries_.clear();sky_.reset();closed_=true;
    }
    // Runtime light allocation failure must not admit additional GPU-only
    // chunks. Existing tokens continue through promotion/persistence.
    void disableNewGeneration(){stopAdmission();}
    size_t count()const noexcept{return entries_.size();}
private:
    void stopAdmission(){if(accepting_){accepting_=false;manager_.set_gpu_generation_producer(6,{});}}
    Chunk_Manager& manager_;VulkanChunkStreamGpu& stream_;VulkanFrameRuntime& runtime_;int32_t dimension_;bool hasSky_;
    VulkanMaterialWorldgen backend_;OwnedWorldgenPreparationQueue prep_;OwnedColumnProducer producer_;
    OwnedChunkPromotionScheduler promotion_;std::unique_ptr<OwnedChunkPersistence> persistence_;
    ResidentSkylightScheduler sky_;std::vector<Entry> entries_;std::vector<ClaimedGpuChunk> candidates_,outside_;
    uint32_t capacity_;WorldResidencyBudget residency_;DeviceMemoryAdmission memoryAdmission_;
    std::vector<VulkanResidentLight::PreviewSkySeed> previewSeeds_;
    uint64_t previewSerial_=0;
    size_t blockCursor_=0,skyCursor_=0;
    bool pressure_=false,accepting_=true,closed_=false;
};
}
