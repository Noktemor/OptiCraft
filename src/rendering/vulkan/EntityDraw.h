#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace OptiCraft::Vulkan {
inline constexpr unsigned EntityBones=64;
struct EntityVertex {
    float position[3],normal[3],uv[2];
    int32_t joints[4];float weights[4];
};
static_assert(sizeof(EntityVertex)==64);
struct EntityTexture {uint32_t width=1,height=1;std::vector<uint8_t> rgba{255,255,255,255};};
struct EntityPrimitive {uint32_t first=0,count=0,texture=0;std::string material;};
struct EntityAsset {
    std::vector<EntityVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<EntityTexture> textures;
    std::vector<EntityPrimitive> primitives;
};
struct alignas(16) EntityPose {
    std::array<float,16> model{};
    std::array<std::array<float,16>,EntityBones> bones{};
    std::array<float,4> options{}; // x skin count, y hurt flash, z ambient
};
static_assert(sizeof(EntityPose)==64+64*EntityBones+16);
struct EntityDraw {std::shared_ptr<const EntityAsset> asset;EntityPose pose;std::string hiddenMaterial;};
}
