#pragma once
#include "utils/configs/Config_World.h"
#include <cstdint>
#include <limits>
namespace OptiCraft::Vulkan {
// Inclusive chunk coordinates. Only physical world edges may be omitted.
struct LightVerticalBounds {
    int32_t low=std::numeric_limits<int32_t>::min(), high=std::numeric_limits<int32_t>::max();
    constexpr bool contains(int64_t y) const noexcept {return y>=low && y<=high;}
};
inline constexpr LightVerticalBounds worldLightBounds{Config::world_min_chunk_y,Config::world_max_chunk_y};
}
