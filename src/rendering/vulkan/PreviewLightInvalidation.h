#pragma once
#include <cstdint>
namespace OptiCraft::Vulkan {
// Block previews consume a one-chunk halo. Changes to light
// alone do not affect their seeds (block IDs + procedural top masks).
template<class Key> constexpr bool previewLightAffected(const Key& core,const Key& changed) noexcept {
    const auto within=[](int32_t a,int32_t b){const int64_t d=int64_t(a)-b;return d>=-1&&d<=1;};
    return core.dimension==changed.dimension&&within(core.x,changed.x)&&within(core.y,changed.y)&&within(core.z,changed.z);
}
// Sky additionally scans contiguous upper resident columns, so a distant
// occluder or newly arrived connector can invalidate any lower core.
template<class Key> constexpr bool previewSkyAffected(const Key& core,const Key& changed) noexcept {
    const auto within=[](int32_t a,int32_t b){const int64_t d=int64_t(a)-b;return d>=-1&&d<=1;};
    return core.dimension==changed.dimension&&within(core.x,changed.x)&&within(core.z,changed.z)&&
           int64_t(changed.y)>=int64_t(core.y)-1;
}
}
