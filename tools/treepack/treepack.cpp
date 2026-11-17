// Offline-only packer: captures the existing Tree_Generator results into one
// versioned, indexed binary file. Runtime never runs this program.
// Usage: ./treepack [assets/trees/trees.tree]
#include "world/generation/Tree_Generator.h"
#include "world/generation/Tree_Pack.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
    struct Block {
        int x, y, z;
        Block_Types id;
    };
    constexpr std::array<const char*, Tree_Pack::k_species_count> names{
        "Oak", "Palm", "Acacia", "Birch", "Spruce", "Jungle", "Slimewood", "Beech",
        "Redwood", "Mangrove", "Willow", "Skywood", "Glowwood", "Baobab", "Cherry", "Aspen"
    };

    void write16(std::ostream& s, uint16_t v) {
        s.put(static_cast<char>(v & 0xffu));
        s.put(static_cast<char>((v >> 8) & 0xffu));
    }
    void write32(std::ostream& s, uint32_t v) {
        write16(s, static_cast<uint16_t>(v));
        write16(s, static_cast<uint16_t>(v >> 16));
    }
    uint64_t mix(uint64_t v) {
        v ^= v >> 30;
        v *= 0xBF58476D1CE4E5B9ull;
        v ^= v >> 27;
        v *= 0x94D049BB133111EBull;
        return v ^ (v >> 31);
    }

    // Prototype of the user's "stick with randomly sized notches": narrow birch
    // trunk, uneven side twigs, small foliage islands. Computed ONLY offline.
    void notched_birch(const Tree_Generator::Block_Setter& put, Fast_Rng& rng) {
        const int height = 8 + static_cast<int>(rng() % 10u);
        std::vector<Block> logs, leaves;
        logs.reserve(64);
        leaves.reserve(200);
        for (int i = 1; i <= height; ++i) logs.push_back({0, i, 0, Block_Types::Birch_Log});
        for (int i = 4; i < height; ++i) {
            if ((rng() % 100u) >= 48u) continue;
            const int direction = static_cast<int>(rng() % 4u);
            const int dx = direction == 0 ? 1 : direction == 1 ? -1 : 0;
            const int dz = direction == 2 ? 1 : direction == 3 ? -1 : 0;
            const int length = 1 + static_cast<int>(rng() % 3u);
            for (int j = 1; j <= length; ++j)
                logs.push_back({dx * j, i + (j > 1 ? 1 : 0), dz * j, Block_Types::Birch_Log});
            const int bx = dx * length, by = i + (length > 1 ? 1 : 0), bz = dz * length;
            const int radius = 1 + static_cast<int>(rng() % 2u);
            for (int ox = -radius; ox <= radius; ++ox)
                for (int oz = -radius; oz <= radius; ++oz)
                    for (int oy = -1; oy <= 1; ++oy) {
                        if (ox * ox + oz * oz + oy * oy > radius * radius + 1) continue;
                        if ((rng() % 100u) < 12u) continue;
                        leaves.push_back({bx + ox, by + oy, bz + oz, Block_Types::Birch_Leaves});
                    }
        }
        for (int oy = -2; oy <= 2; ++oy) {
            const int radius = (oy == -2 || oy == 2) ? 1 : 2;
            for (int ox = -radius; ox <= radius; ++ox)
                for (int oz = -radius; oz <= radius; ++oz)
                    if (ox * ox + oz * oz <= radius * radius && (rng() % 100u) >= 16u)
                        leaves.push_back({ox, height + oy, oz, Block_Types::Birch_Leaves});
        }
        for (const Block& b : leaves) put(b.x, b.y, b.z, b.id);
        for (const Block& b : logs) put(b.x, b.y, b.z, b.id);
    }

    // Slightly irregular conifer tiers. No per-tree procedural construction at runtime.
    void notched_spruce(const Tree_Generator::Block_Setter& put, Fast_Rng& rng) {
        const int height = 9 + static_cast<int>(rng() % 9u);
        std::vector<Block> logs, leaves;
        for (int y = 1; y <= height; ++y) logs.push_back({0, y, 0, Block_Types::Spruce_Log});
        for (int y = 3; y < height; ++y) {
            if (rng() % 100u < 18u) continue;
            const int base = 1 + (height - y) / 4;
            const int r = std::clamp(base + static_cast<int>(rng() % 3u) - 1, 1, 4);
            for (int dx = -r; dx <= r; ++dx)
                for (int dz = -r; dz <= r; ++dz) {
                    if (dx * dx + dz * dz > r * r + 1) continue;
                    if (dx * dx + dz * dz > (r - 1) * (r - 1) && rng() % 100u < 22u) continue;
                    leaves.push_back({dx, y, dz, Block_Types::Spruce_Leaves});
                }
        }
        leaves.push_back({0, height + 1, 0, Block_Types::Spruce_Leaves});
        for (const Block& b : leaves) put(b.x, b.y, b.z, b.id);
        for (const Block& b : logs) put(b.x, b.y, b.z, b.id);
    }

    std::vector<Block> make_variant(Tree_Type species, uint64_t seed) {
        std::vector<Block> result;
        std::unordered_map<uint64_t, std::size_t> locations;
        Fast_Rng rng(seed);
        const auto put = [&](int x, int y, int z, Block_Types id) {
            // Encode signed coordinates into the key without collisions. Last write wins.
            const uint64_t key = (static_cast<uint64_t>(static_cast<uint16_t>(x)) << 32) |
                                 (static_cast<uint64_t>(static_cast<uint16_t>(y)) << 16) |
                                 static_cast<uint16_t>(z);
            auto [it, added] = locations.try_emplace(key, result.size());
            if (added) result.push_back({x, y, z, id});
            else result[it->second].id = id;
        };
        if (species == Tree_Type::Birch) notched_birch(put, rng);
        else if (species == Tree_Type::Spruce) notched_spruce(put, rng);
        else Tree_Generator::place_tree_legacy(species, 0, 0, 0, put, rng);

        // Add a handful of species-coloured leaf "notches" to the original shapes.
        // Several legacy generators have only 3-16 mathematically distinct outputs;
        // the archive must genuinely contain 36 different silhouettes per species.
        constexpr std::array<Block_Types, Tree_Pack::k_species_count> leaf_ids{
            Block_Types::Oak_Leaf, Block_Types::Palm_Leaves, Block_Types::Acacia_Leaves,
            Block_Types::Birch_Leaves, Block_Types::Spruce_Leaves, Block_Types::Jungle_Leaves,
            Block_Types::Slimewood_Leaves, Block_Types::Beech_Leaves, Block_Types::Redwood_Leaves,
            Block_Types::Mangrove_Leaves, Block_Types::Willow_Leaves, Block_Types::Skywood_Leaves,
            Block_Types::Glowwood_Leaves, Block_Types::Baobab_Leaves,
            Block_Types::Cherry_Leaves, Block_Types::Aspen_Leaves
        };
        const auto leaf_id = leaf_ids[static_cast<int>(species)];
        std::vector<std::size_t> leaf_indices;
        for (std::size_t i = 0; i < result.size(); ++i)
            if (result[i].id == leaf_id) leaf_indices.push_back(i);
        if (!leaf_indices.empty()) {
            constexpr int dxs[6] = {1, -1, 0, 0, 0, 0};
            constexpr int dys[6] = {0, 0, 0, 0, 1, -1};
            constexpr int dzs[6] = {0, 0, 1, -1, 0, 0};
            const int count = 3 + static_cast<int>(rng() % 5u);
            int added = 0;
            for (int attempt = 0; attempt < 60 && added < count; ++attempt) {
                const Block& base = result[leaf_indices[rng() % leaf_indices.size()]];
                const int dir = static_cast<int>(rng() % 6u);
                const int nx = base.x + dxs[dir], ny = base.y + dys[dir], nz = base.z + dzs[dir];
                const uint64_t key = (static_cast<uint64_t>(static_cast<uint16_t>(nx)) << 32) |
                                     (static_cast<uint64_t>(static_cast<uint16_t>(ny)) << 16) |
                                     static_cast<uint16_t>(nz);
                if (locations.count(key)) continue;
                put(nx, ny, nz, leaf_id);
                ++added;
            }
        }
        return result;
    }
}

int main(int argc, char** argv) {
    const std::filesystem::path output = argc > 1 ? argv[1] : "assets/trees/trees.tree";
    std::filesystem::create_directories(output.parent_path());
    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    if (!out) { std::cerr << "Cannot open " << output << '\n'; return 1; }
    const uint16_t count = Tree_Pack::k_species_count * Tree_Pack::k_variants_per_species;
    constexpr uint32_t index_begin = 16;
    const uint32_t payload_begin = index_begin + static_cast<uint32_t>(count) * 32u;
    out.write("TREE", 4);
    write16(out, 1); // version
    write16(out, count);
    write32(out, index_begin);
    write32(out, payload_begin);

    struct Index {
        std::string name;
        uint16_t species, variation;
        uint32_t offset, count;
    };
    std::vector<Index> index;
    std::vector<std::vector<Block>> templates;
    index.reserve(count); templates.reserve(count);
    uint64_t cursor = payload_begin;
    std::size_t total_blocks = 0;
    for (int species = 0; species < Tree_Pack::k_species_count; ++species) {
        for (int variant = 0; variant < Tree_Pack::k_variants_per_species; ++variant) {
            const uint64_t seed = mix(0x545245455041434Bull ^
                (static_cast<uint64_t>(species) << 40) ^ static_cast<uint64_t>(variant + 1));
            auto blocks = make_variant(static_cast<Tree_Type>(species), seed);
            if (blocks.empty() || blocks.size() > 20000u ||
                cursor + blocks.size() * 8u > std::numeric_limits<uint32_t>::max()) {
                std::cerr << "Invalid size for " << names[species] << variant + 1 << '\n'; return 2;
            }
            for (const auto& b : blocks) {
                if (b.x < -32768 || b.x > 32767 || b.y < -32768 || b.y > 32767 ||
                    b.z < -32768 || b.z > 32767) {
                    std::cerr << "Out-of-range tree coordinate\n"; return 3;
                }
            }
            index.push_back({std::string(names[species]) + std::to_string(variant + 1),
                             static_cast<uint16_t>(species), static_cast<uint16_t>(variant),
                             static_cast<uint32_t>(cursor), static_cast<uint32_t>(blocks.size())});
            cursor += blocks.size() * 8u;
            total_blocks += blocks.size();
            templates.push_back(std::move(blocks));
        }
    }
    for (const Index& entry : index) {
        std::array<char, 16> name{};
        if (entry.name.size() >= name.size()) return 4;
        entry.name.copy(name.data(), entry.name.size());
        out.write(name.data(), name.size());
        write16(out, entry.species);
        write16(out, entry.variation);
        write32(out, entry.offset);
        write32(out, entry.count);
        write32(out, 0); // reserved
    }
    for (const auto& blocks : templates) for (const Block& b : blocks) {
        write16(out, static_cast<uint16_t>(static_cast<int16_t>(b.x)));
        write16(out, static_cast<uint16_t>(static_cast<int16_t>(b.y)));
        write16(out, static_cast<uint16_t>(static_cast<int16_t>(b.z)));
        write16(out, static_cast<uint16_t>(b.id));
    }
    if (!out.good()) { std::cerr << "Failed to write pack\n"; return 5; }
    std::cout << "TREE v1: " << count << " variants, " << total_blocks
              << " blocks, " << cursor << " bytes -> " << output << '\n';
    return 0;
}
