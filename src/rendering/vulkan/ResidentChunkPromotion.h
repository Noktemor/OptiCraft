#pragma once
#include "rendering/vulkan/VulkanChunkStreamGpu.h"
#include "world/chunks/Chunk.h"

namespace OptiCraft::Vulkan {
// Owner-thread preparation only; this is deliberately NOT a world insertion.
// The caller must reserve the absent CPU slot, prove authoritative GPU ownership
// (not merely gpuBlockLightOwned), and exclude concurrent producers until commit.
// A fresh CPU instance must be registered with the normal lighting/save pipeline.
inline std::unique_ptr<Chunk> prepareResidentChunkPromotion(
    const VulkanChunkStreamGpu& stream,const VulkanChunkStreamGpu::ResidentReadback& snapshot,
    ChunkKey expected,const World_Generator& generator,bool rebuildLight=false) {
    if (snapshot.source.key!=expected || expected.dimension<0 || expected.dimension>UINT16_MAX ||
        !stream.residentReadbackQuiescent(snapshot)) return {};
    const uint32_t zero=0;
    const auto light=rebuildLight?std::span<const uint32_t>(&zero,1):std::span<const uint32_t>(snapshot.words[2]);
    auto result=Chunk::from_resident_words(expected.x,expected.y,expected.z,
        static_cast<Dimension_Id>(expected.dimension),generator,
        snapshot.words[0],snapshot.words[1],light);
    if (!stream.residentReadbackQuiescent(snapshot)) return {};
    return result;
}
} // namespace OptiCraft::Vulkan
