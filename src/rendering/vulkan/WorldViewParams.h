#pragma once
#include <array>
#include <cstddef>
namespace OptiCraft::Vulkan {
struct alignas(16) WorldViewParams {
    std::array<float,16> vp{};
    std::array<float,4> eyeAmbient{}; // xyz eye, w sky ambient
    std::array<float,4> fogColorStart{}; // rgb fog, w start
    std::array<float,4> fogEnd{}; // x fog end; yzw directional sunlight vector (zero = disabled)
    std::array<float,4> heldLight{}; // RGB tint, w emission radius in blocks
};
static_assert(sizeof(WorldViewParams)==128);
static_assert(offsetof(WorldViewParams,eyeAmbient)==64);
static_assert(offsetof(WorldViewParams,fogColorStart)==80);
static_assert(offsetof(WorldViewParams,fogEnd)==96);
static_assert(offsetof(WorldViewParams,heldLight)==112);
}
