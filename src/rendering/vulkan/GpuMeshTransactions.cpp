#include "rendering/vulkan/GpuMeshTransactions.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace OptiCraft::Vulkan {
GpuMeshTransactions::GpuMeshTransactions(uint32_t slots, uint64_t opaqueBytes,
                                         uint64_t miscBytes, uint32_t batch)
    : slots_(slots), opaque_(opaqueBytes), misc_(miscBytes), maxBatch_(batch) {
    // Shader indices are uint32, therefore buffers cannot exceed 2^32 records.
    if (!slots || !batch || !opaqueBytes || !miscBytes ||
        opaqueBytes / 8 > UINT32_MAX || miscBytes / 32 > UINT32_MAX)
        throw std::invalid_argument("Invalid GPU mesh arena/scheduler dimensions");
    jobs_.reserve(batch);
}
void GpuMeshTransactions::dirty(uint32_t slot, uint32_t generation) {
    if (slot >= slots_.size() || !generation) throw std::out_of_range("dirty GPU slot");
    auto& s = slots_[slot];
    if (!s.active || s.generation != generation) {
        defer(s.opaque, s.misc);
        s.opaque = {}; s.misc = {};
        s.active = true;
        s.generation = generation;
        s.revision = 0;
        s.pending = false;
    }
    if (++s.revision == 0) ++s.revision;
    if (!s.pending) {s.pending = true; pending_.push_back(slot);}
}
void GpuMeshTransactions::evict(uint32_t slot, uint32_t generation) {
    if (slot >= slots_.size()) throw std::out_of_range("evict GPU slot");
    auto& s = slots_[slot];
    if (!s.active || s.generation != generation) return; // stale unload
    defer(s.opaque, s.misc);
    s.opaque = {}; s.misc = {}; s.active = false; s.pending = false;
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
    if (phase_!=Phase::Idle) return false;
    jobs_.clear();
    while (!pending_.empty() && jobs_.size() < maxBatch_) {
        uint32_t slot=pending_.front();pending_.pop_front();
        auto& s=slots_[slot];
        if (!s.active || !s.pending) continue;
        s.pending=false;
        jobs_.push_back({slot,s.generation,s.revision});
    }
    if (jobs_.empty()) return false;
    phase_=Phase::Counting;
    return true;
}
bool GpuMeshTransactions::finishCount(std::span<const uint32_t> opaqueCounts,
                                      std::span<const uint32_t> miscCounts) {
    if (phase_!=Phase::Counting || opaqueCounts.size()!=jobs_.size() ||
        miscCounts.size()!=jobs_.size()) throw std::logic_error("Unexpected count result");
    bool any=false;
    for (size_t i=0;i<jobs_.size();++i) {
        auto& j=jobs_[i];
        if (!current(j)) {j.eligible=false; ++stats_.stale;continue;}
        j.opaqueCount=opaqueCounts[i];j.miscCount=miscCounts[i];
        if (uint64_t(j.opaqueCount)*8 > opaque_.capacity() ||
            uint64_t(j.miscCount)*32 > misc_.capacity()) {
            j.eligible=false; ++stats_.overflow;continue;
        }
        if (j.opaqueCount) {
            auto allocated=opaque_.allocate(uint64_t(j.opaqueCount)*8,16);
            if (!allocated) {j.eligible=false; ++stats_.allocationFailures;continue;}
            j.opaqueCandidate=*allocated;
        }
        if (j.miscCount) {
            auto allocated=misc_.allocate(uint64_t(j.miscCount)*32,32);
            if (!allocated) {
                discard(j);j.eligible=false;++stats_.allocationFailures;continue;
            }
            j.miscCandidate=*allocated;
        }
        any=true;
    }
    // Failed jobs are retried on a future invalidation or when memory budget
    // becomes available. No endless per-frame GPU polling of an OOM chunk.
    if (!any) {jobs_.clear();phase_=Phase::Idle;return false;}
    phase_=Phase::Emitting;
    return true;
}
std::vector<GpuMeshTransactions::DrawChange>
GpuMeshTransactions::verifyEmit(std::span<const uint32_t> opaqueCounts,
                                std::span<const uint32_t> miscCounts) {
    if (phase_!=Phase::Emitting || opaqueCounts.size()!=jobs_.size() ||
        miscCounts.size()!=jobs_.size()) throw std::logic_error("Unexpected emit result");
    std::vector<DrawChange> result;
    result.reserve(jobs_.size());
    for(size_t i=0;i<jobs_.size();++i){
        Entry& j=jobs_[i];
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
    if (phase_!=Phase::Emitting || changes.size()!=jobs_.size())
        throw std::logic_error("Invalid publish transaction");
    for(size_t i=0;i<jobs_.size();++i){
        Entry& j=jobs_[i];
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
    jobs_.clear();phase_=Phase::Idle;
}
void GpuMeshTransactions::completedFrame() {
    ++completedFrames_;
    // Conservative release: at least three *successfully waited* frame-slot
    // fences have passed after retirement. Never uses vkDeviceWaitIdle here.
    auto it=std::remove_if(retirement_.begin(),retirement_.end(),[this](const Deferred& d){
        if(d.readyFrame>completedFrames_)return false;
        if(d.opaque && !opaque_.release(d.opaque))throw std::logic_error("Stale opaque range");
        if(d.misc && !misc_.release(d.misc))throw std::logic_error("Stale misc range");
        return true;
    });
    retirement_.erase(it,retirement_.end());
}
void GpuMeshTransactions::cancelInFlight() {
    // Called after all GPU fences have completed (shutdown only).
    for(auto& j:jobs_)discard(j);
    jobs_.clear();phase_=Phase::Idle;
}
} // namespace OptiCraft::Vulkan
