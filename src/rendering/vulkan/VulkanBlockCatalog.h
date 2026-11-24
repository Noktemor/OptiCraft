#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

class Block_Registry;

namespace OptiCraft::Vulkan {

// GPU material ABI v1. An entry exists for every possible uint16 Block_ID.
// Only definitions from the actual game's registry are allowed to render.
// The catalog never builds geometry on the CPU.
enum VisualFlags : uint32_t {
    VisualOpaqueCube = 1u << 0,
    VisualAlphaCube  = 1u << 1,
    VisualFlora      = 1u << 2,
    VisualLiquid     = 1u << 3,
    VisualShaped     = 1u << 4,
    VisualOccluder   = 1u << 5,
    VisualMeta       = 1u << 6,
    VisualBlended    = 1u << 7, // translucent, not alpha-cutout
};
inline constexpr uint32_t kVisualKindMask = VisualOpaqueCube | VisualAlphaCube |
    VisualFlora | VisualLiquid | VisualShaped;

struct alignas(16) GpuBlockVisual {
    uint32_t faceBase = 0;   // index in GpuFaceUv array
    uint32_t metaStride = 0; // 0 or 6; address = faceBase + meta * metaStride + face
    uint32_t shape = 0;
    uint32_t flags = 0;
};
static_assert(sizeof(GpuBlockVisual) == 16);

struct alignas(16) GpuFaceUv {
    std::array<float, 4> uv{}; // umin,vmin,umax,vmax (same orientation as game atlas)
    std::array<uint32_t, 4> options{}; // x: quarter-turn flag, remaining reserved
};
static_assert(sizeof(GpuFaceUv) == 32);

// Each box uses 7-bit 1/64-block coordinates: all 6 endpoints in [0,64].
// x: lo.x | lo.y<<7 | lo.z<<14, y: hi.x | hi.y<<7 | hi.z<<14.
struct GpuShapeBox { uint32_t lo = 0, hi = 0; };
struct GpuShapeIndex { uint32_t first = 0, count = 0; };
static_assert(sizeof(GpuShapeBox) == 8 && sizeof(GpuShapeIndex) == 8);

struct GpuBlockCatalog {
    std::vector<GpuBlockVisual> blocks;
    // Stage 2 opaque mesher has a distinct 2-bit ABI: 1=full cube, 2=occluder.
    // Upload this array to voxel_count/emit materialFlags, NOT blocks[].flags.
    std::vector<uint32_t> opaqueMesherFlags;
    std::vector<GpuFaceUv> faces;
    std::vector<GpuShapeIndex> shapeIndex; // [shape * 256 + metadata]
    std::vector<GpuShapeBox> shapeBoxes;
    std::array<uint32_t, 5> activeKinds{}; // opaque, alpha, flora, liquid, shaped
    uint32_t activeBlockDefinitions = 0;

    // Run after Block_Registry is loaded. Block/texture metadata are read once;
    // GPU compute/vertex shaders use these immutable SSBOs for every chunk.
    static GpuBlockCatalog build(const Block_Registry& registry);
};

} // namespace OptiCraft::Vulkan
