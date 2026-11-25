#pragma once
#include "rendering/vulkan/ResidentChunkPromotion.h"
#include "world/chunks/Chunk_Manager.h"

namespace OptiCraft::Vulkan {
// Called only by a future authoritative GPU-world owner. Reserving CPU space
// does not make a renderer mirror authoritative. Keep the source alive and
// exclude concurrent producers until this function returns success; then the
// caller must transfer authority and stop GPU-only mutations of this key.
inline bool commitResidentChunkPromotion(Chunk_Manager& manager,uint64_t ticket,
    const VulkanChunkStreamGpu& stream,const VulkanChunkStreamGpu::ResidentReadback& snapshot,bool rebuildLight=false) {
    const auto* generator=manager.world_generator();
    if (!ticket || !generator) return false;
    auto prepared=prepareResidentChunkPromotion(stream,snapshot,snapshot.source.key,*generator,rebuildLight);
    if (!prepared) return false;
    if(rebuildLight)manager.prepare_gpu_promotion_lighting(*prepared);
    return manager.commit_gpu_chunk_promotion(ticket,std::move(prepared),[&]{
        return stream.residentReadbackQuiescent(snapshot);
    });
}
} // namespace OptiCraft::Vulkan
