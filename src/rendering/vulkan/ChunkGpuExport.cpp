#include "rendering/vulkan/ChunkGpuExport.h"
#include "world/chunks/Chunk.h"
#include "world/blocks/Block_Types.h"

#include <array>
#include <stdexcept>

namespace OptiCraft::Vulkan {
void exportCpuChunkBlocks(const Chunk& chunk, std::span<uint32_t> words) {
    std::array<Block_ID, kChunkVoxels> blocks{};
    chunk.get_blocks().unpack(blocks.data());
    packBlockIds(blocks, words);
}

std::array<uint32_t, 65536> exportOpaqueCubeFlags(const Block_Registry& registry) {
    std::array<uint32_t, 65536> flags{};
    // Do not give unknown block IDs geometry or occlusion properties.
    for (size_t i = 1; i < registry.id_capacity() && i < flags.size(); ++i) {
        const auto id = static_cast<Block_ID>(i);
        if (!registry.is_known(id)) continue;
        const Block_Properties& p = registry.get_properties(id);
        if (p.mesh_type == Block_Mesh_Type::Solid && p.is_opaque && !p.is_transparent)
            flags[i] = 3u;
    }
    return flags;
}
} // namespace OptiCraft::Vulkan
