#include "world/generation/Tree_Pack.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <utility>

namespace {
    constexpr std::size_t k_header_bytes = 16;
    constexpr std::size_t k_index_bytes = 32;
    constexpr std::size_t k_block_bytes = 8;
    constexpr std::size_t k_max_file_bytes = 64u * 1024u * 1024u;
    constexpr uint32_t k_max_blocks_per_template = 20000u;

    uint16_t u16(const std::vector<uint8_t>& b, std::size_t off) {
        return static_cast<uint16_t>(b[off]) |
               (static_cast<uint16_t>(b[off + 1]) << 8);
    }
    uint32_t u32(const std::vector<uint8_t>& b, std::size_t off) {
        return static_cast<uint32_t>(b[off]) |
               (static_cast<uint32_t>(b[off + 1]) << 8) |
               (static_cast<uint32_t>(b[off + 2]) << 16) |
               (static_cast<uint32_t>(b[off + 3]) << 24);
    }
    int16_t i16(const std::vector<uint8_t>& b, std::size_t off) {
        const uint16_t bits = u16(b, off);
        return bits < 0x8000u ? static_cast<int16_t>(bits) :
            static_cast<int16_t>(static_cast<int32_t>(bits) - 65536);
    }
    int floor_div(int x, int d) {
        const int q = x / d, r = x % d;
        return q - (r < 0 ? 1 : 0);
    }
    uint64_t mix64(uint64_t x) {
        x ^= x >> 30;
        x *= 0xBF58476D1CE4E5B9ull;
        x ^= x >> 27;
        x *= 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    }
    // A spatial, order-independent cycle through 36 choices in a 6x6 cell tile.
    // Region-specific offset decorrelates tiles; parallel worker scheduling does not
    // affect the result, even when coordinates are negative.
    int choose_variant(int x, int z, int species, uint64_t seed) {
        constexpr int cell_width = 16;
        constexpr int side = 6;
        const int cell_x = floor_div(x, cell_width);
        const int cell_z = floor_div(z, cell_width);
        const int tile_x = floor_div(cell_x, side);
        const int tile_z = floor_div(cell_z, side);
        const int local_x = cell_x - tile_x * side;
        const int local_z = cell_z - tile_z * side;
        uint64_t h = mix64(seed ^ (static_cast<uint64_t>(species) << 48));
        h = mix64(h ^ static_cast<uint64_t>(static_cast<int64_t>(tile_x)) * 0x9E3779B185EBCA87ull);
        h = mix64(h ^ static_cast<uint64_t>(static_cast<int64_t>(tile_z)) * 0xC2B2AE3D27D4EB4Full);
        const int offset = static_cast<int>(h % Tree_Pack::k_variants_per_species);
        return (local_z * side + local_x + offset) % Tree_Pack::k_variants_per_species;
    }
}

const Tree_Pack& Tree_Pack::instance() {
    static const Tree_Pack pack;
    return pack;
}

Tree_Pack::Tree_Pack() {
    // Prototypes run by default from CLion. Explicit "off" is a rollback switch.
    const char* mode = std::getenv("OPTICRAFT_TREE_PACK");
    if (mode && (std::strcmp(mode, "off") == 0 || std::strcmp(mode, "0") == 0)) return;
    m_enabled = true;

    // World/asset system uses the working directory (same as assets/blocks.json).
    std::ifstream file("assets/trees/trees.tree", std::ios::binary | std::ios::ate);
    if (!file) return;
    const auto length = file.tellg();
    if (length < static_cast<std::streamoff>(k_header_bytes) ||
        length > static_cast<std::streamoff>(k_max_file_bytes)) return;
    std::vector<uint8_t> bytes(static_cast<std::size_t>(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()))) return;

    if (bytes[0] != 'T' || bytes[1] != 'R' || bytes[2] != 'E' || bytes[3] != 'E' ||
        u16(bytes, 4) != 1u) return;
    const uint16_t count = u16(bytes, 6);
    const uint32_t index_begin = u32(bytes, 8);
    const uint32_t payload_begin = u32(bytes, 12);
    if (count == 0 || count > k_species_count * k_variants_per_species ||
        index_begin < k_header_bytes ||
        static_cast<uint64_t>(index_begin) + count * k_index_bytes > bytes.size() ||
        payload_begin < index_begin + count * k_index_bytes ||
        payload_begin > bytes.size()) return;

    // Decode into temporary storage. A malformed file must not expose a half-loaded pack.
    decltype(m_templates) decoded;
    std::array<std::array<bool, k_variants_per_species>, k_species_count> seen{};
    for (uint16_t i = 0; i < count; ++i) {
        const std::size_t e = static_cast<std::size_t>(index_begin) + i * k_index_bytes;
        const uint16_t species = u16(bytes, e + 16);
        const uint16_t variant = u16(bytes, e + 18);
        const uint32_t offset = u32(bytes, e + 20);
        const uint32_t blocks = u32(bytes, e + 24);
        if (species >= k_species_count || variant >= k_variants_per_species ||
            seen[species][variant] || blocks == 0 || blocks > k_max_blocks_per_template ||
            offset < payload_begin ||
            static_cast<uint64_t>(offset) + static_cast<uint64_t>(blocks) * k_block_bytes > bytes.size()) return;
        seen[species][variant] = true;

        // Each archive entry has a readable, null-terminated name, e.g. Birch1.
        bool terminated = false;
        for (std::size_t c = 0; c < 16; ++c) {
            if (bytes[e + c] == 0) { terminated = c > 0; break; }
            if (bytes[e + c] < '!' || bytes[e + c] > '~') return;
        }
        if (!terminated) return;

        auto& dest = decoded[species][variant];
        dest.reserve(blocks);
        for (uint32_t n = 0; n < blocks; ++n) {
            const std::size_t p = static_cast<std::size_t>(offset) + n * k_block_bytes;
            dest.push_back(Block{i16(bytes, p), i16(bytes, p + 2), i16(bytes, p + 4),
                                 static_cast<Block_Types>(u16(bytes, p + 6))});
        }
    }
    m_templates = std::move(decoded);
    m_count = count;
    m_valid = true;
}

bool Tree_Pack::place(Tree_Type species, int x, int y, int z,
                      const Tree_Generator::Block_Setter& setter, uint64_t world_seed) const {
    if (!m_valid) return false;
    const int id = static_cast<int>(species);
    if (id < 0 || id >= k_species_count) return false;
    const int variant = choose_variant(x, z, id, world_seed);
    const auto& tree = m_templates[id][variant];
    if (tree.empty()) return false;
    for (const Block& block : tree) {
        setter(x + block.x, y + block.y, z + block.z, block.type);
    }
    return true;
}
