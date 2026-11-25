#pragma once
#include <cstdint>
namespace OptiCraft::Vulkan {
// LOD1 merges coplanar opaque geometry regardless of texture/light. Original
// voxel IDs, metadata and illumination are reconstructed in the fragment stage.
// Hysteresis prevents remeshing on every crossing of a camera chunk boundary.
inline constexpr uint32_t kSurfaceLodFlag=16u;
inline constexpr uint32_t kGeometryLodFlag=512u; // bits 5..8 are voxel address flags
inline constexpr uint32_t kFarGeometryLodFlag=1024u;
inline constexpr uint32_t kLod2Flags=kSurfaceLodFlag|kGeometryLodFlag;
inline constexpr uint32_t kLodFlags=kLod2Flags|kFarGeometryLodFlag;
inline constexpr bool surfaceLod(double distanceSquared,bool current) noexcept {
    return distanceSquared >= (current ? 64.0 : 100.0);
}
// LOD2 is deliberately conservative; thresholds measured in chunk units.
inline constexpr bool geometryLod(double distanceSquared,bool current) noexcept {
    return distanceSquared >= (current ? 144.0 : 196.0);
}
inline constexpr uint32_t selectSurfaceLod(double d,uint32_t previous,bool opaque,bool eligible,bool enabled,bool geometryEnabled) noexcept {
    if(!enabled || !opaque || !surfaceLod(d,(previous&kSurfaceLodFlag)!=0))return 0;
    if(!geometryEnabled || !eligible || !geometryLod(d,(previous&kGeometryLodFlag)!=0))return kSurfaceLodFlag;
    return kLod2Flags | (d >= ((previous&kFarGeometryLodFlag)?324.0:400.0)?kFarGeometryLodFlag:0u);
}
}
