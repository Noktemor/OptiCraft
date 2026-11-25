#pragma once
#include "rendering/vulkan/OwnedChunkPromotion.h"
#include "world/chunks/Gpu_World_Streaming_Policy.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace OptiCraft::Vulkan {
// Owner thread; one instance per world/stream. After stream.record and BOTH
// lighting schedulers' observe/publication, before the render pass. Only this
// client may transition its active tokens or consume their readbacks. Submit
// the frame normally; after an aborted frame use authorized idle teardown.
// Caller retains the long-lived registry, removes only acknowledged releases,
// and drains all attempts (disable admission) before replacing the world.
class OwnedChunkPromotionScheduler {
    using Stream=VulkanChunkStreamGpu;
    using State=Stream::OwnershipState;
    struct Attempt {
        ClaimedGpuChunk chunk;
        Stream::PromotionToken token;
        enum Phase {Reading,Draining,Retiring,CancelOnly} phase=Reading;
    };
public:
    static constexpr size_t probeBudget=64, reservationBudget=8;
    struct Result {
        bool accepted=false,recorded=false,committed=false,blocked=false;
        size_t canceled=0;
        std::array<std::optional<ClaimedGpuChunk>,Stream::kFrameSlots> released{};
    };
    OwnedChunkPromotionScheduler(Chunk_Manager& manager,Stream& stream):manager_(manager),stream_(stream){}
    OwnedChunkPromotionScheduler(const OwnedChunkPromotionScheduler&)=delete;
    OwnedChunkPromotionScheduler& operator=(const OwnedChunkPromotionScheduler&)=delete;
    // GPU idle, no future polling; the stream teardown retires any readbacks.
    void cancelIdle(){
        for(auto& a:attempts_)if(a){manager_.cancel_gpu_chunk_promotion(a->token.cpuTicket);stream_.cancelOwnedPromotion(a->token);a.reset();}
    }
    size_t activeCount() const noexcept {
        return std::count_if(attempts_.begin(),attempts_.end(),[](const auto& a){return a.has_value();});
    }
    // Same near CPU+guard region as the generation policy. The outer window
    // also matters: a teleport out of the window must cancel, not promote.
    static bool wanted(const Gpu_World_Streaming::Window& w,int32_t dimension,ChunkKey k) noexcept {
        if(k.dimension!=dimension || w.render_radius<Config::render_distance_min ||
           w.render_radius>Config::render_distance_max || w.cpu_radius<Config::render_distance_min ||
           w.vertical_half<0 || w.vertical_half>Config::world_height_chunks ||
           k.y<Config::world_min_chunk_y || k.y>Config::world_max_chunk_y)return false;
        const auto dx=distance(k.x,w.x),dz=distance(k.z,w.z),dy=distance(k.y,w.y);
        if(dx>w.render_radius || dz>w.render_radius || dy>w.vertical_half ||
           std::max(dx,dz)>int64_t(w.cpu_radius)+1)return false;
        return Chunk_Stream_Window::wanted(w.render_radius,w.vertical_half,int(dx),int(dy),int(dz));
    }
    // Strict default: both recordedCurrent gates must be true. They cover
    // current committed dependencies, not a fence. The explicit CPU-relight
    // policy instead discards both captured light channels at reconstruction
    // if either gate is missing; it never certifies provisional GPU light.
    template<class BlockScheduler,class SkyScheduler>
    Result pump(VkCommandBuffer cmd,uint32_t slot,const StreamBatch& batch,
                const Gpu_World_Streaming::Window& window,int32_t dimension,
                std::span<const ClaimedGpuChunk> candidates,const BlockScheduler& block,
                const SkyScheduler& sky,bool admit=true,bool allowCpuRelight=false){
        Result out;
        if(slot>=attempts_.size() || !stream_.currentStreamFrame(cmd,slot,batch) ||
           batch.streamRecordSerial==lastSerial_)return out;
        lastSerial_=batch.streamRecordSerial;out.accepted=true;
        const auto exact=[&](ChunkKey key){return block.recordedCurrent(stream_,batch,key) && sky.recordedCurrent(stream_,batch,key);};
        const auto ready=[&](const ClaimedGpuChunk& c){
            const auto key=c.owner().key;
            return wanted(window,dimension,key) && claimedChunkCurrent(manager_,c) &&
                (allowCpuRelight || exact(key));
        };
        for(size_t i=0;i<attempts_.size();++i){
            auto& a=attempts_[i];if(!a)continue;
            if(a->phase==Attempt::Retiring){
                if(!stream_.ownershipState(a->chunk.owner())){
                    if(releaseRetiredGpuChunk(manager_,stream_,a->chunk)){out.released[i]=a->chunk;a.reset();}
                    else out.blocked=true;
                }
                continue;
            }
            if(a->phase==Attempt::CancelOnly){
                if(cancel(*a,out))a.reset();
                continue;
            }
            if(a->phase==Attempt::Reading && !ready(a->chunk)){
                if(!cancel(*a,out))continue;
            }
            if(i!=slot)continue; // only this slot's fence has been acknowledged
            auto snapshot=stream_.takeOwnedReadback(slot,a->token);
            if(!snapshot){out.blocked=true;continue;}
            if(a->phase==Attempt::Draining){a.reset();continue;}
            if(ready(a->chunk) && stream_.residentReadbackQuiescent(*snapshot) &&
               commitOwnedChunkPromotion(manager_,stream_,a->chunk,a->token,*snapshot,!exact(a->chunk.owner().key))){
                a->phase=Attempt::Retiring;out.committed=true;retryOffset_=0;continue;
            }
            // Failed CPU pool/storage/identity validation: return authority to
            // GPU, recapture on a later pass, never replay a stale snapshot.
            a->phase=Attempt::CancelOnly;++retryOffset_;
            if(cancel(*a,out))a.reset();
        }
        // At most one CPU reconstruction OR one new capture per invocation.
        // On failure/cancellation give the lighting and CPU queues a pass.
        if(!admit || attempts_[slot] || out.committed || out.canceled || out.blocked || candidates.empty())return out;
        struct Candidate {size_t index;int64_t horizontal,vertical;};
        std::array<Candidate,probeBudget> choices{};size_t count=0;
        const size_t probes=std::min(candidates.size(),probeBudget);
        cursor_%=candidates.size();
        for(size_t i=0;i<probes;++i){
            const size_t index=(cursor_+i)%candidates.size();const auto& c=candidates[index];
            if(c.owner().owner!=&stream_ || !ready(c) || stream_.ownershipState(c.owner())!=State::Resident)continue;
            if(std::any_of(attempts_.begin(),attempts_.end(),[&](const auto& a){return a && a->chunk.owner()==c.owner();}))continue;
            const auto k=c.owner().key;
            choices[count++]={index,std::max(distance(k.x,window.x),distance(k.z,window.z)),distance(k.y,window.y)};
        }
        cursor_=(cursor_+probes)%candidates.size();
        std::sort(choices.begin(),choices.begin()+count,[](const auto& a,const auto& b){
            if(a.horizontal!=b.horizontal)return a.horizontal<b.horizontal;
            if(a.vertical!=b.vertical)return a.vertical<b.vertical;
            return a.index<b.index;
        });
        if(!count)return out;
        // Rotate failed admissions/commits within the prioritized probe set,
        // so a permanently refused nearest coordinate cannot monopolize it.
        retryOffset_%=count;
        const size_t tries=std::min(count,reservationBudget);
        for(size_t i=0;i<tries;++i){
            const auto& c=candidates[choices[(retryOffset_+i)%count].index];
            auto token=beginOwnedChunkPromotion(manager_,stream_,c);if(!token)continue;
            attempts_[slot].emplace(Attempt{c,*token});
            if(stream_.recordOwnedReadback(cmd,slot,*token)){out.recorded=true;retryOffset_+=i;return out;}
            attempts_[slot]->phase=Attempt::CancelOnly;
            if(cancel(*attempts_[slot],out))attempts_[slot].reset();
            retryOffset_+=i+1;return out;
        }
        retryOffset_+=tries;return out;
    }
private:
    static constexpr int64_t distance(int32_t a,int32_t b) noexcept {
        const auto d=int64_t(a)-b;return d<0?-d:d;
    }
    bool cancel(Attempt& a,Result& out){
        if(!cancelOwnedChunkPromotion(manager_,stream_,a.chunk,a.token)){out.blocked=true;return false;}
        a.phase=Attempt::Draining;++out.canceled;return true;
    }
    Chunk_Manager& manager_;Stream& stream_;
    std::array<std::optional<Attempt>,Stream::kFrameSlots> attempts_{};
    uint64_t lastSerial_=0;size_t cursor_=0,retryOffset_=0;
};
} // namespace OptiCraft::Vulkan
