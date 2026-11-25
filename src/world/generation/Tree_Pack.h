#ifndef OPTICRAFT_TREE_PACK_H
#define OPTICRAFT_TREE_PACK_H

// Immutable tree template data, loaded once before the chunk workers start.
// This is a data source for Tree_Generator, NOT a chunk/job/render pipeline.
#include <array>
#include <cstdint>
#include <vector>
#include <span>
#include "world/generation/Tree_Generator.h"

class Tree_Pack {
public:
    static constexpr int k_species_count = 16;
    static constexpr int k_variants_per_species = 36;

    // Thread-safe C++ static initialization; later placement only reads arrays.
    static const Tree_Pack& instance();
    bool enabled() const { return m_enabled; }
    bool valid() const { return m_valid; }
    std::size_t template_count() const { return m_count; }

    // Returns false if the archive is unavailable/invalid or the species is absent.
    // No disk I/O, allocation, RNG mutation or synchronization in the hot path.
    bool place(Tree_Type species, int x, int y, int z,
               const Tree_Generator::Block_Setter& setter, uint64_t world_seed) const;

    // Borrowed immutable archive view; empty means use the legacy generator.
    struct Block {
        int16_t x, y, z;
        Block_Types type;
    };
    std::span<const Block> selected_template(Tree_Type species, int x, int z,
                                             uint64_t world_seed) const;
    std::span<const Block> template_at(Tree_Type species, int variant) const noexcept;
    static int variant_at(Tree_Type species, int x, int z, uint64_t world_seed);

private:
    using Template = std::vector<Block>;
    std::array<std::array<Template, k_variants_per_species>, k_species_count> m_templates{};
    bool m_enabled = false;
    bool m_valid = false;
    std::size_t m_count = 0;

    Tree_Pack();
};

#endif // OPTICRAFT_TREE_PACK_H
