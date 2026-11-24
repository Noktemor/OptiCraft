#pragma once

#include "rendering/vulkan/GpuWorldCore.h"
#include <array>
#include <cstdint>
#include <span>

class Chunk;
class Block_Registry;

namespace OptiCraft::Vulkan {

// Only serialization of original blocks for the CPU-authoritative zone.
// Does not perform face culling, meshing, LOD or drawing on CPU.
void exportCpuChunkBlocks(const Chunk& chunk, std::span<uint32_t> words);

// flags bit0 = simple opaque-cube geometry eligible for GPU quad mesher;
// flags bit1 = occludes other simple cube faces.
// Flora, liquids, shaped and translucent blocks require separate GPU pipelines.
std::array<uint32_t, 65536> exportOpaqueCubeFlags(const Block_Registry& registry);

} // namespace OptiCraft::Vulkan
