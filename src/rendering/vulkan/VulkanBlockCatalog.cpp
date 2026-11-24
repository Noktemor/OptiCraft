#include "rendering/vulkan/VulkanBlockCatalog.h"
#include "world/blocks/Block_Types.h"
#include "world/blocks/Block_Shapes.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace OptiCraft::Vulkan {
namespace {
uint32_t quantize(float coordinate) {
    if (!std::isfinite(coordinate)) throw std::runtime_error("Block shape has nonfinite extent");
    const long cell = std::lround(double(coordinate) * 64.0);
    if (cell < 0 || cell > 64 || std::abs(double(coordinate) * 64.0 - cell) > 0.001)
        throw std::runtime_error("Block shape coordinate cannot be encoded in 1/64 block");
    return uint32_t(cell);
}
uint32_t packCorner(const glm::vec3& corner) {
    return quantize(corner.x) | (quantize(corner.y) << 7u) | (quantize(corner.z) << 14u);
}
} // namespace

GpuBlockCatalog GpuBlockCatalog::build(const Block_Registry& registry) {
    GpuBlockCatalog result;
    result.blocks.resize(k_max_block_types); // GPU index always equals the game's 16-bit ID
    result.opaqueMesherFlags.resize(k_max_block_types);
    result.shapeIndex.resize(size_t(Block_Shape::Count) * 256);
    result.faces.reserve(registry.id_capacity() * 6);

    // The shape table is the same source of truth as physics and OpenGL geometry.
    // Generate it once on load, never on every voxel or frame. Effective stair
    // corners / fence connections must be computed from neighboring GPU blocks.
    for (uint32_t shape = 1; shape < uint32_t(Block_Shape::Count); ++shape) {
        for (uint32_t meta = 0; meta < 256; ++meta) {
            auto& index = result.shapeIndex[shape * 256 + meta];
            index.first = uint32_t(result.shapeBoxes.size());
            Shape_Box boxes[16]{};
            const int count = shape_boxes(static_cast<Block_Shape>(shape), uint8_t(meta), boxes);
            if (count < 0 || count > 16)
                throw std::runtime_error("Block shape table exceeded 16 boxes");
            index.count = uint32_t(count);
            for (int i = 0; i < count; ++i) {
                for (int axis = 0; axis < 3; ++axis) {
                    if (boxes[i].lo[axis] >= boxes[i].hi[axis])
                        throw std::runtime_error("Block shape has nonpositive extent");
                }
                result.shapeBoxes.push_back({packCorner(boxes[i].lo), packCorner(boxes[i].hi)});
            }
        }
    }

    for (uint32_t id = 1; id < registry.id_capacity() && id < k_max_block_types; ++id) {
        if (!registry.is_known(static_cast<Block_ID>(id))) continue;
        const Block_Properties& p = registry.get_properties(static_cast<Block_ID>(id));
        if (p.is_item) continue; // inventory item, not a placed world voxel
        uint32_t kind = 0;
        uint32_t kindIndex = 0;
        switch (p.mesh_type) {
            case Block_Mesh_Type::Solid:
                if (p.mesh_style == Block_Mesh_Style::XStyle) {
                    kind = VisualFlora; kindIndex = 2;
                } else if (p.is_opaque && !p.is_transparent) {
                    kind = VisualOpaqueCube; kindIndex = 0;
                } else {
                    kind = VisualAlphaCube; kindIndex = 1;
                }
                break;
            case Block_Mesh_Type::Flora: kind = VisualFlora; kindIndex = 2; break;
            case Block_Mesh_Type::Liquid: kind = VisualLiquid; kindIndex = 3; break;
            case Block_Mesh_Type::Shaped: kind = VisualShaped; kindIndex = 4; break;
            default: throw std::runtime_error("Unknown mesh type in actual block catalog");
        }
        if (kind == VisualShaped && (p.shape == Block_Shape::None || p.shape >= Block_Shape::Count))
            throw std::runtime_error("Shaped block has no valid shape ID");
        auto& desc = result.blocks[id];
        desc.faceBase = uint32_t(result.faces.size());
        desc.shape = uint32_t(p.shape);
        desc.flags = kind | ((kind == VisualOpaqueCube) ? VisualOccluder : 0u);
        // NEVER upload VisualFlags to voxel_count/emit: bit 1 there means
        // occlusion, whereas in this catalog it means transparent cube.
        result.opaqueMesherFlags[id] = (kind == VisualOpaqueCube) ? 3u : 0u;
        const bool metaDependent = p.uses_meta || p.orientation != Block_Orientation::None || p.has_active_state;
        if (metaDependent) desc.flags |= VisualMeta;
        // Alpha-cutout flora stays in the depth-writing cutout pass.
        if (kind == VisualLiquid || kind == VisualAlphaCube ||
            (kind == VisualShaped && p.is_transparent && !p.is_opaque))
            desc.flags |= VisualBlended;
        desc.metaStride = metaDependent ? 6u : 0u;
        const uint32_t variants = metaDependent ? 256u : 1u;
        for (uint32_t meta = 0; meta < variants; ++meta) {
            for (uint32_t face = 0; face < 6; ++face) {
                bool rotated = false;
                const glm::vec4& uv = p.resolve_face_uv(int(face), uint8_t(meta), rotated);
                result.faces.push_back({{uv.x, uv.y, uv.z, uv.w}, {uint32_t(rotated), 0, 0, 0}});
            }
        }
        ++result.activeKinds[kindIndex];
        ++result.activeBlockDefinitions;
    }
    return result;
}
} // namespace OptiCraft::Vulkan
