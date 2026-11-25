#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>
#include <span>
#include <vector>

// Direct skylight ABI: each column stores first lit local Y (0..32).
// 32 means fully dark; 0 means open from the bottom. The CPU still handles
// world bounds and cross-chunk propagation. Column order is z,x.
namespace Generation_Sky {
inline constexpr size_t columns = 1024, transparency_words = 2048;
inline constexpr size_t input_words = columns + transparency_words;
inline constexpr uint32_t dispatch_mode = 0x80000000u;
// Optional packed seed volume, in the same y,z,x byte layout as the voxel arena.
inline constexpr size_t emission_words = 8192, seed_words = 8192;
inline constexpr size_t seed_input_words = input_words + emission_words + 1;
inline constexpr uint32_t seed_dispatch_mode = 0x80000001u;
inline constexpr bool has_seeds(size_t n) noexcept { return n == seed_input_words; }
inline constexpr bool valid_input(size_t n) noexcept { return n == 0 || n == input_words || has_seeds(n); }
inline constexpr size_t output_words(size_t n) noexcept {
    return n == 0 ? 0 : columns + (has_seeds(n) ? seed_words : 0);
}
inline void prepare_seeds(std::vector<uint32_t>& input, const uint8_t* emission,
                          int base_y, int min_y, int max_y) {
    if (input.size() != input_words) return;
    input.resize(seed_input_words, 0u);
    for (size_t id=0; id<65536; ++id)
        input[input_words + id/8] |= uint32_t(std::min<uint8_t>(emission[id],14u)) << (4*(id%8));
    for (unsigned y=0; y<32; ++y) {
        const int64_t wy=int64_t(base_y)+y;
        if (wy>=min_y && wy<=max_y) input.back() |= 1u<<y;
    }
}
inline constexpr uint32_t cutoff(int terrain_top, int base_y, int local_top) noexcept {
    // Use wide subtraction even when a caller supplies extreme coordinates.
    const int64_t relative = int64_t(terrain_top) - base_y + 1;
    return static_cast<uint32_t>(std::clamp(std::max(relative, int64_t(local_top)+1), int64_t(0), int64_t(32)));
}
inline void prepare(std::vector<uint32_t>& input, const int* terrain, int base_y,
                    int min_y, int max_y, const bool* transparent) {
    input.assign(input_words, 0u);
    for(size_t c=0;c<columns;++c)
        input[c]=cutoff(std::clamp(terrain[c], min_y-1, max_y),base_y,-1);
    for(size_t id=0;id<65536;++id)
        if(transparent[id]) input[columns+id/32] |= 1u<<(id%32);
}
// Independently validate GPU output against the authoritative CPU heightmap.
inline bool matches(std::span<const uint32_t> gpu, std::span<const uint32_t> cpu) noexcept {
    return gpu.size()==columns && cpu.size()==columns && std::equal(gpu.begin(),gpu.end(),cpu.begin());
}
}
