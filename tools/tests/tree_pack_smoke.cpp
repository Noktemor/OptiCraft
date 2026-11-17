// Optional C++ smoke test, links Tree_Generator.cpp + Tree_Pack.cpp without the game.
// Run from project root; tree packs are enabled by default, validates read-only placement.
#include "world/generation/Tree_Generator.h"
#include "world/generation/Tree_Pack.h"

#include <cassert>
#include <cstdint>
#include <algorithm>
#include <iostream>
#include <thread>
#include <vector>

namespace {
    uint64_t hash_tree(Tree_Type type, int x, int z, uint64_t seed) {
        Fast_Rng rng(0x123456789ABCDEF0ull);
        uint64_t h = 14695981039346656037ull;
        unsigned blocks = 0;
        const auto setter = [&](int bx, int by, int bz, Block_Types b) {
            const uint64_t x1 = static_cast<uint64_t>(static_cast<uint32_t>(bx - x));
            const uint64_t z1 = static_cast<uint64_t>(static_cast<uint32_t>(bz - z));
            const uint64_t y1 = static_cast<uint64_t>(static_cast<uint32_t>(by));
            h = (h ^ x1) * 1099511628211ull;
            h = (h ^ y1) * 1099511628211ull;
            h = (h ^ z1) * 1099511628211ull;
            h = (h ^ static_cast<uint16_t>(b)) * 1099511628211ull;
            ++blocks;
        };
        Tree_Generator::place_tree(type, x, 0, z, setter, rng, seed);
        assert(blocks > 0);
        return h;
    }
}

int main() {
    const Tree_Pack& pack = Tree_Pack::instance();
    if (!pack.valid()) {
        // Disabled or corrupted/missing archive must preserve the legacy generator.
        Fast_Rng rng(123);
        int blocks = 0;
        Tree_Generator::place_tree(Tree_Type::Birch, 0, 0, 0,
                                   [&](int, int, int, Block_Types) { ++blocks; }, rng, 1327);
        assert(blocks > 0);
        std::cout << "tree pack fallback passed: procedural Birch, " << blocks << " writes\n";
        return 0;
    }
    assert(pack.enabled());
    assert(pack.template_count() == 16 * 36);

    // 6x6 spatial cycle must expose all 36 templates for each species, regardless
    // of calls' order. Hashes include complete placement geometry.
    for (int species = 0; species < 16; ++species) {
        std::vector<uint64_t> hashes;
        for (int z = -3; z < 3; ++z)
            for (int x = -3; x < 3; ++x)
                hashes.push_back(hash_tree(static_cast<Tree_Type>(species), x * 16, z * 16, 1327));
        // These 36 points cross regions; their individual output stays stable.
        for (int z = -3; z < 3; ++z)
            for (int x = -3; x < 3; ++x)
                assert(hashes[(z + 3) * 6 + x + 3] ==
                    hash_tree(static_cast<Tree_Type>(species), x * 16, z * 16, 1327));
        // All 36 choices must appear in the same (nonnegative) 6x6 region.
        std::vector<uint64_t> tile;
        for (int z = 0; z < 6; ++z)
            for (int x = 0; x < 6; ++x)
                tile.push_back(hash_tree(static_cast<Tree_Type>(species), x * 16, z * 16, 1327));
        std::sort(tile.begin(), tile.end());
        assert(std::unique(tile.begin(), tile.end()) == tile.end());
    }

    std::vector<uint64_t> baseline(128);
    for (int n = 0; n < 128; ++n)
        baseline[n] = hash_tree(static_cast<Tree_Type>(n % 16), -800 + n * 21, 450 - n * 11, 1327);
    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t) workers.emplace_back([&baseline] {
        for (int n = 0; n < 128; ++n)
            assert(baseline[n] == hash_tree(static_cast<Tree_Type>(n % 16),
                                          -800 + n * 21, 450 - n * 11, 1327));
    });
    for (auto& worker : workers) worker.join();
    std::cout << "tree pack smoke passed: 16 species, 576 variants, 8 workers\n";
}
