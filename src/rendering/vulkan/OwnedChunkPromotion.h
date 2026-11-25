#pragma once
#include "rendering/vulkan/ResidentChunkPromotionCommit.h"

namespace OptiCraft::Vulkan {
// Owner-thread transaction spanning the CPU scheduler and GPU streamer.
// Keep this token until retirement is acknowledged; canceling a readback
// never releases CPU generation exclusion. No implicit discard on destruction.
class ClaimedGpuChunk {
    Chunk_Manager* manager_;
    VulkanChunkStreamGpu::OwnershipToken owner_;
    uint64_t claim_;
    ClaimedGpuChunk(Chunk_Manager* manager,VulkanChunkStreamGpu::OwnershipToken owner,uint64_t claim)
        :manager_(manager),owner_(owner),claim_(claim){}
    friend std::optional<ClaimedGpuChunk> submitClaimedGpuChunk(Chunk_Manager&,VulkanChunkStreamGpu&,uint64_t,LiveChunkPacket&&);
public:
    Chunk_Manager* manager() const {return manager_;}
    VulkanChunkStreamGpu::OwnershipToken owner() const {return owner_;}
    uint64_t claim() const {return claim_;}
};
inline glm::ivec3 claimedPosition(const ClaimedGpuChunk& chunk) {
    return {chunk.owner().key.x,chunk.owner().key.y,chunk.owner().key.z};
}
inline bool claimedChunkCurrent(Chunk_Manager& manager,const ClaimedGpuChunk& chunk) {
    return chunk.manager()==&manager && manager.gpu_chunk_claim_current(claimedPosition(chunk),chunk.claim());
}
// Reserve before scheduling procedural GPU work. Caller retains the ticket
// while generating; cancellation of unsubmitted work uses cancel_gpu_chunk_claim.
// Failed source admission rolls back a bound claim; failed bind leaves the
// pending claim available for explicit cancellation. No CPU fallback races.
inline std::optional<ClaimedGpuChunk> submitClaimedGpuChunk(Chunk_Manager& manager,
    VulkanChunkStreamGpu& stream,uint64_t claim,LiveChunkPacket&& packet) {
    const auto key=packet.position;
    if(key.dimension<0 || key.dimension>UINT16_MAX)return std::nullopt;
    const glm::ivec3 position{key.x,key.y,key.z};
    if(!manager.bind_gpu_chunk_claim(position,static_cast<Dimension_Id>(key.dimension),claim))return std::nullopt;
    try {
        auto owner=stream.submitOwnedGpuSnapshot(std::move(packet));
        if(owner)return ClaimedGpuChunk{&manager,*owner,claim};
    } catch(...) {
        manager.release_gpu_chunk_claim(position,claim);
        throw;
    }
    manager.release_gpu_chunk_claim(position,claim);
    return std::nullopt;
}
// Convenience path for an already available source; two-phase producers
// should call claim_gpu_chunk before generation instead.
inline std::optional<ClaimedGpuChunk> admitOwnedChunk(Chunk_Manager& manager,
    VulkanChunkStreamGpu& stream,LiveChunkPacket&& packet) {
    const auto key=packet.position;
    if(key.dimension<0 || key.dimension>UINT16_MAX)return std::nullopt;
    const glm::ivec3 position{key.x,key.y,key.z};
    const auto claim=manager.claim_gpu_chunk(position,static_cast<Dimension_Id>(key.dimension));
    if(!claim)return std::nullopt;
    try {
        auto owned=submitClaimedGpuChunk(manager,stream,claim,std::move(packet));
        if(!owned)manager.cancel_gpu_chunk_claim(position,claim);
        return owned;
    } catch(...) {
        // A bind failure may throw before the claim became bound.
        manager.cancel_gpu_chunk_claim(position,claim);
        throw;
    }
}
inline std::optional<VulkanChunkStreamGpu::PromotionToken> beginOwnedChunkPromotion(
    Chunk_Manager& manager,VulkanChunkStreamGpu& stream,const ClaimedGpuChunk& chunk) {
    const auto key=chunk.owner().key;
    if(!claimedChunkCurrent(manager,chunk) ||
       stream.ownershipState(chunk.owner())!=VulkanChunkStreamGpu::OwnershipState::Resident)return std::nullopt;
    const auto ticket=manager.reserve_gpu_chunk_promotion(claimedPosition(chunk),
        static_cast<Dimension_Id>(key.dimension),chunk.claim());
    if(!ticket)return std::nullopt;
    auto token=stream.beginOwnedPromotion(chunk.owner(),ticket);
    if(!token)manager.cancel_gpu_chunk_promotion(ticket);
    return token;
}
inline bool cancelOwnedChunkPromotion(Chunk_Manager& manager,VulkanChunkStreamGpu& stream,
    const ClaimedGpuChunk& chunk,VulkanChunkStreamGpu::PromotionToken token) {
    if(!claimedChunkCurrent(manager,chunk) || token.source!=chunk.owner() ||
       !stream.cancelOwnedPromotion(token))return false;
    // Manager may already have canceled a reservation after camera movement.
    manager.cancel_gpu_chunk_promotion(token.cpuTicket);
    return true;
}
inline bool commitOwnedChunkPromotion(Chunk_Manager& manager,VulkanChunkStreamGpu& stream,
    const ClaimedGpuChunk& chunk,VulkanChunkStreamGpu::PromotionToken token,
    const VulkanChunkStreamGpu::ResidentReadback& snapshot,bool rebuildLight=false) {
    if(!claimedChunkCurrent(manager,chunk) || token.source!=chunk.owner())return false;
    return stream.commitOwnedPromotion(token,snapshot,[&]{
        return commitResidentChunkPromotion(manager,token.cpuTicket,stream,snapshot,rebuildLight);
    });
}
// Call only for reproducible or durably saved data; the stream remains protected
// until record() acknowledges retirement. A promoted source uses the same ack.
inline bool discardClaimedGpuChunk(Chunk_Manager& manager,VulkanChunkStreamGpu& stream,
    const ClaimedGpuChunk& chunk) {
    return claimedChunkCurrent(manager,chunk) && stream.discardOwnedChunk(chunk.owner());
}
inline bool releaseRetiredGpuChunk(Chunk_Manager& manager,VulkanChunkStreamGpu& stream,
    const ClaimedGpuChunk& chunk) {
    // Call after a successful stream record/retirement (or authorized idle
    // teardown). A different stream is not proof of retirement.
    if(!claimedChunkCurrent(manager,chunk) || chunk.owner().owner!=&stream ||
       stream.ownershipState(chunk.owner()))return false;
    return manager.release_gpu_chunk_claim(claimedPosition(chunk),chunk.claim());
}
} // namespace OptiCraft::Vulkan
