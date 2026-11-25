#include "rendering/vulkan/GpuMeshTransactions.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace OptiCraft::Vulkan {
GpuMeshTransactions::GpuMeshTransactions(uint32_t slots, uint64_t opaqueBytes,
                                         uint64_t miscBytes, uint32_t batch, uint32_t workBudget, uint32_t batchCount)
    : slots_(slots), batches_(batchCount), opaque_(opaqueBytes), misc_(miscBytes), maxBatch_(batch),
      workBudget_(workBudget?workBudget:batch*8u) {
    // Shader indices are uint32, therefore buffers cannot exceed 2^32 records.
    if (!slots || !batch || !batchCount || !opaqueBytes || !miscBytes ||
        opaqueBytes / 8 > UINT32_MAX || miscBytes / 32 > UINT32_MAX)
        throw std::invalid_argument("Invalid GPU mesh arena/scheduler dimensions");
    for(auto& b:batches_)b.jobs.reserve(batch);
}
void GpuMeshTransactions::selectBatch(uint32_t index) {
    if(index>=batches_.size())throw std::out_of_range("GPU mesh batch index");
    selectedBatch_=index;
}
void GpuMeshTransactions::releaseBatch() {
    for(const auto& j:batch().jobs)slots_[j.slot].inFlight=false;
    batch().jobs.clear();batch().phase=Phase::Idle;
}
void GpuMeshTransactions::enqueue(uint32_t slot){
    auto& s=slots_[slot];
    if(s.pending)return;
    s.pendingToken=++nextPendingToken_;
    pending_.push_back({slot,s.generation,s.pendingToken});
    s.pending=true;++pendingCount_;
}
void GpuMeshTransactions::cancelPending(Resident& s) noexcept{
    if(s.pending){s.pending=false;--pendingCount_;}
}
void GpuMeshTransactions::dirty(uint32_t slot, uint32_t generation, uint32_t workCost) {
    if (slot >= slots_.size() || !generation) throw std::out_of_range("dirty GPU slot");
    auto& s = slots_[slot];
    if (!s.active || s.generation != generation) {
        defer(s.opaque, s.misc);
        s.opaque = {}; s.misc = {};
        s.active = true;
        if(s.generation!=generation)s.revision=0;
        s.generation = generation;
        cancelPending(s);
    }
    s.workCost=std::max(1u,workCost);
    if (++s.revision == 0) ++s.revision;
    s.retryOnRelease=false;
    s.forceLegacy=false;
    enqueue(slot);
}
void GpuMeshTransactions::clearEmpty(uint32_t slot, uint32_t generation) {
    if(slot>=slots_.size() || !generation)throw std::out_of_range("empty GPU slot");
    auto& s=slots_[slot];
    defer(s.opaque,s.misc);
    s.opaque={};s.misc={};s.active=false;cancelPending(s);s.retryOnRelease=false;s.generation=generation;
    if(++s.revision==0)++s.revision;
}
void GpuMeshTransactions::evict(uint32_t slot, uint32_t generation) {
    if (slot >= slots_.size()) throw std::out_of_range("evict GPU slot");
    auto& s = slots_[slot];
    if (!s.active || s.generation != generation) return; // stale unload
    defer(s.opaque, s.misc);
    s.opaque = {}; s.misc = {}; s.active = false; cancelPending(s); s.retryOnRelease=false;
    if (++s.revision == 0) ++s.revision;
}
bool GpuMeshTransactions::current(const Entry& j) const {
    const auto& s = slots_[j.slot];
    return s.active && s.generation == j.generation && s.revision == j.revision;
}
void GpuMeshTransactions::defer(BufferRange a, BufferRange b) {
    if (a || b) retirement_.push_back({a,b,completedFrames_+3});
}
void GpuMeshTransactions::discard(Entry& j) {
    // These candidates are not visible in published GPU draw commands. They
    // nevertheless need to wait for the previous emit submission's fence.
    if (j.opaqueCandidate && !opaque_.release(j.opaqueCandidate))
        throw std::logic_error("Candidate opaque range double release");
    if (j.miscCandidate && !misc_.release(j.miscCandidate))
        throw std::logic_error("Candidate misc range double release");
    j.opaqueCandidate={};j.miscCandidate={};
}
bool GpuMeshTransactions::startCount() {
    if (batch().phase!=Phase::Idle) return false;
    batch().jobs.clear();
    uint64_t work=0;
    size_t scan=pending_.size();
    while (scan-- && !pending_.empty() && batch().jobs.size() < maxBatch_) {
        const auto pending=pending_.front();
        const uint32_t slot=pending.slot;
        auto& s=slots_[slot];
        if (!s.active || !s.pending || s.generation!=pending.generation || s.pendingToken!=pending.token) {pending_.pop_front();continue;}
        if(s.inFlight){pending_.pop_front();pending_.push_back(pending);continue;}
        if(!batch().jobs.empty() && work+s.workCost>workBudget_)break;
        pending_.pop_front();
        work+=s.workCost;
        cancelPending(s);
        s.inFlight=true;
        batch().jobs.push_back({slot,s.generation,s.revision});
        batch().jobs.back().workCost=s.workCost;
        batch().jobs.back().forceLegacy=s.forceLegacy;
    }
    if (batch().jobs.empty()) return false;
    batch().phase=Phase::Counting;
    return true;
}
bool GpuMeshTransactions::finishCount(std::span<const uint32_t> opaqueCounts,
                                      std::span<const uint32_t> miscCounts) {
    if (batch().phase!=Phase::Counting || batch().fastSlab || opaqueCounts.size()!=batch().jobs.size() ||
        miscCounts.size()!=batch().jobs.size()) throw std::logic_error("Unexpected count result");
    bool any=false;
    for (size_t i=0;i<batch().jobs.size();++i) {
        auto& j=batch().jobs[i];
        if (!current(j)) {j.eligible=false; ++stats_.stale;continue;}
        j.opaqueCount=opaqueCounts[i];j.miscCount=miscCounts[i];
        if (uint64_t(j.opaqueCount)*8 > opaque_.capacity() ||
            uint64_t(j.miscCount)*32 > misc_.capacity()) {
            j.eligible=false; ++stats_.overflow;continue;
        }
        if (j.opaqueCount) {
            auto allocated=opaque_.allocate(uint64_t(j.opaqueCount)*8,16);
            if (!allocated) {j.eligible=false; slots_[j.slot].retryOnRelease=true; ++stats_.allocationFailures;continue;}
            j.opaqueCandidate=*allocated;
        }
        if (j.miscCount) {
            auto allocated=misc_.allocate(uint64_t(j.miscCount)*32,32);
            if (!allocated) {
                discard(j);j.eligible=false;slots_[j.slot].retryOnRelease=true;++stats_.allocationFailures;continue;
            }
            j.miscCandidate=*allocated;
        }
        any=true;
    }
    // Failed jobs are retried on a future invalidation or when memory budget
    // becomes available. No endless per-frame GPU polling of an OOM chunk.
    if (!any) {releaseBatch();return false;}
    batch().phase=Phase::Emitting;
    return true;
}
BufferRange GpuMeshTransactions::reserveFastOpaque(uint32_t capacity, uint32_t miscCapacity) {
    auto& b=batch();
    if(b.phase!=Phase::Counting || b.fastSlab || !capacity ||
       std::any_of(b.jobs.begin(),b.jobs.end(),[](const Entry& j){return j.forceLegacy;}))return {};
    // Round each worst-case job to 16-byte alignment. Slabs use the same arena
    // as ordinary candidates, so simultaneous batches cannot alias live draws.
    const uint64_t stride=(uint64_t(capacity)+1u)&~uint64_t(1);
    if(stride>opaque_.capacity()/8/b.jobs.size()){++stats_.fastReservationMisses;return {};}
    auto range=opaque_.allocate(stride*8*b.jobs.size(),16);
    if(!range){++stats_.fastReservationMisses;return {};}
    if(miscCapacity){
        if(uint64_t(miscCapacity)>misc_.capacity()/32/b.jobs.size()){
            if(!opaque_.release(*range))throw std::logic_error("Invalid fast rollback");
            ++stats_.fastReservationMisses;return {};
        }
        auto miscRange=misc_.allocate(uint64_t(miscCapacity)*32*b.jobs.size(),32);
        if(!miscRange){
            if(!opaque_.release(*range))throw std::logic_error("Invalid fast rollback");
            ++stats_.fastReservationMisses;return {};
        }
        b.fastMiscSlab=*miscRange;b.fastMiscCapacity=miscCapacity;
    }
    b.fastSlab=*range;b.fastCapacity=capacity;++stats_.fastBatches;
    return *range;
}
void GpuMeshTransactions::finishFastOpaque(std::span<const uint32_t> counts,
                                           std::span<const FastDraw> draws,
                                           std::span<const uint32_t> miscCounts) {
    auto& b=batch();
    if(b.phase!=Phase::Counting || !b.fastSlab || counts.size()!=b.jobs.size() || draws.size()!=b.jobs.size() || (b.fastMiscSlab ? miscCounts.size()!=b.jobs.size() : !miscCounts.empty()))
        throw std::logic_error("Unexpected fast opaque result");
    // Reconstruct the shader's deterministic prefix allocation. Validate every
    // returned indirect command before transferring slab ownership to chunks.
    uint64_t cursor=b.fastSlab.offset;
    bool validBatch=true;
    for(size_t i=0;i<b.jobs.size();++i){
        const uint32_t n=counts[i];
        if(n>b.fastCapacity){validBatch &= draws[i]==FastDraw{};continue;}
        const FastDraw expected{6,n,0,uint32_t(cursor/8)};
        validBatch &= draws[i]==expected;
        cursor+=((uint64_t(n)+1u)&~uint64_t(1))*8;
    }
    validBatch &= cursor<=b.fastSlab.offset+b.fastSlab.size;
    std::vector<BufferRange> retained, retainedMisc;retained.reserve(b.jobs.size());
    retainedMisc.reserve(b.jobs.size());
    cursor=b.fastSlab.offset;
    for(size_t i=0;i<b.jobs.size();++i){
        auto& j=b.jobs[i];const uint32_t n=counts[i];
        const bool fits=n<=b.fastCapacity;
        const uint32_t m=b.fastMiscSlab?miscCounts[i]:0u;
        const bool miscFits=m<=b.fastMiscCapacity;
        j.eligible=false;
        if(!current(j)){++stats_.stale;}
        else if(!validBatch || !fits || !miscFits){
            if(!miscFits)++stats_.fastMiscOverflow;
            // Same revision must use the exact legacy path, not retry the cache forever.
            slots_[j.slot].forceLegacy=true;enqueue(j.slot);++stats_.fastFallbacks;
        }else{
            j.eligible=true;j.opaqueCount=n;j.miscCount=m;
            // Preserve even the zero-count base for verifyEmit/indirect ABI.
            j.opaqueCandidate={cursor,uint64_t(n)*8};
            if(n)retained.push_back(j.opaqueCandidate);
            if(b.fastMiscSlab){
                // The shader emits directly into its own bounded fixed-stride region.
                // Keep only the actual prefix, returning all unused capacity.
                j.miscCandidate={b.fastMiscSlab.offset+uint64_t(i)*b.fastMiscCapacity*32,uint64_t(m)*32};
                if(m)retainedMisc.push_back(j.miscCandidate);
                ++stats_.fastMixedJobs;
            }
            ++stats_.fastJobs;
        }
        if(fits)cursor+=((uint64_t(n)+1u)&~uint64_t(1))*8;
    }
    const auto usedBefore=opaque_.used();
    if(!opaque_.partition(b.fastSlab,retained))throw std::logic_error("Invalid fast opaque partition");
    if(opaque_.used()<usedBefore)reclaimedSinceFrame_=true;
    if(b.fastMiscSlab){
        const auto miscBefore=misc_.used();
        if(!misc_.partition(b.fastMiscSlab,retainedMisc))throw std::logic_error("Invalid fast misc partition");
        if(misc_.used()<miscBefore)reclaimedSinceFrame_=true;
    }
    b.fastSlab={};b.fastMiscSlab={};b.fastCapacity=0;b.fastMiscCapacity=0;b.phase=Phase::Emitting;
}
std::vector<GpuMeshTransactions::DrawChange>
GpuMeshTransactions::verifyEmit(std::span<const uint32_t> opaqueCounts,
                                std::span<const uint32_t> miscCounts) {
    if (batch().phase!=Phase::Emitting || opaqueCounts.size()!=batch().jobs.size() ||
        miscCounts.size()!=batch().jobs.size()) throw std::logic_error("Unexpected emit result");
    std::vector<DrawChange> result;
    result.reserve(batch().jobs.size());
    for(size_t i=0;i<batch().jobs.size();++i){
        Entry& j=batch().jobs[i];
        DrawChange d{j.slot,j.generation,j.opaqueCount,j.miscCount,
                     uint32_t(j.opaqueCandidate.offset/8),uint32_t(j.miscCandidate.offset/32),false};
        if (j.eligible && current(j) && opaqueCounts[i]==j.opaqueCount &&
            miscCounts[i]==j.miscCount) {
            d.valid=true;
        } else {
            if (j.eligible) ++stats_.stale;
            discard(j);
        }
        result.push_back(d);
    }
    return result;
}
void GpuMeshTransactions::acceptPublish(std::span<const DrawChange> changes) {
    if (batch().phase!=Phase::Emitting || changes.size()!=batch().jobs.size())
        throw std::logic_error("Invalid publish transaction");
    for(size_t i=0;i<batch().jobs.size();++i){
        Entry& j=batch().jobs[i];
        const DrawChange& d=changes[i];
        if(d.slot!=j.slot || d.generation!=j.generation)
            throw std::invalid_argument("Wrong GPU publish slot/generation");
        if (d.valid) {
            if (!j.eligible || !current(j) || d.opaqueCount!=j.opaqueCount ||
                d.miscCount!=j.miscCount || d.opaqueBase!=j.opaqueCandidate.offset/8 ||
                d.miscBase!=j.miscCandidate.offset/32)
                throw std::logic_error("Invalid GPU publish candidate");
            Resident& s=slots_[j.slot];
            defer(s.opaque,s.misc);
            s.opaque=j.opaqueCandidate;s.misc=j.miscCandidate;
            j.opaqueCandidate={};j.miscCandidate={};
            ++stats_.published;
        }
        discard(j);
    }
    releaseBatch();
}
void GpuMeshTransactions::completedFrame() {
    ++completedFrames_;
    // Conservative release: at least three *successfully waited* frame-slot
    // fences have passed after retirement. Never uses vkDeviceWaitIdle here.
    bool reclaimed=reclaimedSinceFrame_;reclaimedSinceFrame_=false;
    auto it=std::remove_if(retirement_.begin(),retirement_.end(),[this,&reclaimed](const Deferred& d){
        if(d.readyFrame>completedFrames_)return false;
        if(d.opaque && !opaque_.release(d.opaque))throw std::logic_error("Stale opaque range");
        if(d.misc && !misc_.release(d.misc))throw std::logic_error("Stale misc range");
        reclaimed=true;
        return true;
    });
    retirement_.erase(it,retirement_.end());
    // Only retry after actual retirement returns GPU memory, not every frame.
    // A failed allocation must not leave a still-resident chunk permanently blank.
    if(reclaimed)for(uint32_t i=0;i<slots_.size();++i){
        auto& s=slots_[i];
        if(!s.active || !s.retryOnRelease)continue;
        s.retryOnRelease=false;
        enqueue(i);++stats_.allocationRetries;
    }
}
size_t GpuMeshTransactions::pendingCount() const noexcept {
    return pendingCount_;
}
uint32_t GpuMeshTransactions::batchWork(bool eligibleOnly) const noexcept {
    uint32_t cost=0;
    for(const auto& j:batch().jobs)if(!eligibleOnly || (j.eligible && current(j)))cost+=j.workCost;
    return cost;
}
void GpuMeshTransactions::cancelInFlight() {
    // Called after all GPU fences have completed (shutdown only).
    const auto previous=selectedBatch_;
    for(uint32_t i=0;i<batches_.size();++i){
        selectBatch(i);for(auto& j:batch().jobs)discard(j);
        if(batch().fastSlab && !opaque_.release(batch().fastSlab))throw std::logic_error("Invalid canceled fast slab");
        if(batch().fastMiscSlab && !misc_.release(batch().fastMiscSlab))throw std::logic_error("Invalid canceled fast misc slab");
        batch().fastSlab={};batch().fastMiscSlab={};batch().fastCapacity=0;batch().fastMiscCapacity=0;releaseBatch();
    }
    selectBatch(previous);
}
} // namespace OptiCraft::Vulkan
