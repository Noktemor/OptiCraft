#ifndef OPTICRAFT_GPU_WORLDGEN_DATA_H
#define OPTICRAFT_GPU_WORLDGEN_DATA_H

#include <array>
#include <cstdint>
#include <vector>

// Matches std430 layout in assets/shaders/worldgen_material.comp.
struct alignas(8) Gpu_Material_Column {
    double surface_height = 0.0;
    uint32_t biome = 0;
    uint32_t padding = 0;
};
static_assert(sizeof(Gpu_Material_Column) == 16);

struct alignas(32) Gpu_Lattice_Node {
    double delta[4]{};          // dvec4, xyz used
    int32_t offsets[4]{};       // ivec4, xyz used
    int32_t next[4]{};          // ivec4: failure, success; -1 terminates
};
static_assert(sizeof(Gpu_Lattice_Node) == 64);

struct Gpu_Noise_Tables {
    std::array<int32_t, 2048> permutation{};
    std::array<double, 2048 * 4> gradients{};
    std::vector<Gpu_Lattice_Node> nodes;
    std::array<int32_t, 8> starts{};
};

#endif
