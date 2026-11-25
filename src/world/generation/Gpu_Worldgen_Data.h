#ifndef OPTICRAFT_GPU_WORLDGEN_DATA_H
#define OPTICRAFT_GPU_WORLDGEN_DATA_H

#include <array>
#include <cstdint>
#include <cstddef>
#include <limits>
#include <vector>

// Matches std430 layout in assets/shaders/worldgen_material.comp.
struct alignas(8) Gpu_Material_Column {
    double surface_height = 0.0;
    uint32_t biome = 0;
    uint32_t padding = 0;
};
static_assert(sizeof(Gpu_Material_Column) == 16);

// Vulkan binding 5: nine reserved Y indices followed by this payload.
// Header word 1 packs water ID (low 16) and world-floor mantle ID (high 16).
// Four words per X/Z column: signed river bed/water/top, packed surface/mantle IDs.
// Cave membership uses two bits per voxel, in dispatched-level order.
struct Gpu_Base_Region {
    static constexpr size_t levels_reserved = 9;
    static constexpr size_t columns = 1024;
    static constexpr size_t voxels = 32768;
    static constexpr size_t cave_words = voxels / 16;
    static constexpr size_t header_words = 2;
    static constexpr size_t profile_words = columns * 4;
    static constexpr size_t cave_offset = header_words + profile_words;
    static constexpr uint32_t fallback = 0xffffu; // base mode only; 0 is real air
    static constexpr uint32_t enabled = 1u, sky = 2u;
    static constexpr size_t words(size_t count) { return cave_offset + count * cave_words; }
    std::vector<uint32_t> data;
    void reset(size_t count, int sea, uint16_t water, uint16_t floor_mantle = 0) {
        data.assign(words(count), 0u);
        data[0] = static_cast<uint32_t>(sea); data[1] = uint32_t(water) | (uint32_t(floor_mantle) << 16u);
    }
    void profile(size_t col, bool river, int bed, int water, int top,
                 uint16_t surface, uint16_t mantle) {
        const size_t p = header_words + col * 4;
        data[p] = static_cast<uint32_t>(river ? bed : std::numeric_limits<int32_t>::max());
        data[p+1] = static_cast<uint32_t>(water); data[p+2] = static_cast<uint32_t>(top);
        data[p+3] = uint32_t(surface) | (uint32_t(mantle) << 16u);
    }
    void cave(size_t level, size_t voxel, uint32_t kind) {
        const size_t p = cave_offset + level * cave_words + voxel / 16;
        const unsigned shift = unsigned(voxel % 16) * 2;
        data[p] = (data[p] & ~(3u << shift)) | ((kind & 3u) << shift);
    }
};

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
