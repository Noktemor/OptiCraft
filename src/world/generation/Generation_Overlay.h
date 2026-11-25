#pragma once

#include "utils/configs/Config_World.h"
#include "world/blocks/Block_Types.h"
#include "world/generation/Generation_Overlay_Packet.h"
#include "world/generation/Gpu_Tree_Overlay_Data.h"
#include "world/generation/Gpu_Tree_Overlay_Catalog.h"
#include "world/generation/Gpu_Tree_Coverage.h"
#include <cstdint>
#include <vector>

// Ordered, chunk-local placement commands. The generator evaluates all terrain
// predicates against the procedural base, not against preceding decorations.
// Keep repeated writes (including Air and meta=0): same-ID writes preserve meta,
// changed-ID writes reset it, and later structures may overwrite earlier flora.
// GPU ABI: bit 31 = metadata command, bits 16..30 = y,z,x linear index,
// bits 0..15 = block ID or metadata value. No padding, no sentinel block ID.
class Generation_Overlay {
    static_assert(Config::chunk_size == 32 && Config::chunk_height == 32);
    int m_x, m_y, m_z;
    std::vector<uint32_t>& m_words;
    std::vector<Gpu_Tree_Overlay::Instance>* m_trees=nullptr;
    std::vector<Gpu_Tree_Overlay::Selection>* m_selections=nullptr;
    size_t m_native_candidates=0, m_native_candidate_words=0;
    static constexpr bool inside(int x, int y, int z) {
        return x >= 0 && x < 32 && y >= 0 && y < 32 && z >= 0 && z < 32;
    }
    static constexpr uint32_t address(int x, int y, int z) {
        return (uint32_t(y) << 10u) | (uint32_t(z) << 5u) | uint32_t(x);
    }
public:
    Generation_Overlay(int x, int y, int z, std::vector<uint32_t>& storage,
                       std::vector<Gpu_Tree_Overlay::Instance>* trees=nullptr,
                       std::vector<Gpu_Tree_Overlay::Selection>* selections=nullptr)
        : m_x(x), m_y(y), m_z(z), m_words(storage), m_trees(trees),m_selections(selections) { m_words.clear(); if(m_trees)m_trees->clear(); if(m_selections)m_selections->clear(); }
    template<class Predicate>
    bool capture_tree(Tree_Type type,int x,int y,int z,uint64_t seed,Predicate&& allow,uint32_t biome=0xffffffffu,bool defer_air=false){
        if(!m_trees||Gpu_Tree_Overlay::catalog_words().empty())return false;
        const auto species=uint32_t(type);
        if(defer_air&&m_selections&&biome<39&&seed<=UINT32_MAX&&species<Gpu_Tree_Overlay::templates/36&&
           x>=INT32_MIN+128&&x<=INT32_MAX-128&&z>=INT32_MIN+128&&z<=INT32_MAX-128){
            const auto before=m_trees->size();
            m_selections->reserve(m_selections->size()+1);
            if(Gpu_Tree_Overlay::append_variant_candidate(m_words,*m_trees,Gpu_Tree_Overlay::species_coverage_catalog()[species],species,
                    int64_t(x)-int64_t(m_x)*32,int64_t(y)-int64_t(m_y)*32,int64_t(z)-int64_t(m_z)*32)){
                if(m_trees->size()>before)m_selections->push_back({uint32_t(before),x,z,uint32_t(seed),Gpu_Tree_Overlay::variant_only});
                return true;
            }
        }
        const int variant=Tree_Pack::variant_at(type,x,z,seed);
        const auto tree=Tree_Pack::instance().template_at(type,variant);
        if(variant<0||tree.empty())return false;
        const int64_t ox=int64_t(m_x)*32,oy=int64_t(m_y)*32,oz=int64_t(m_z)*32;
        const auto before=m_trees->size();
        const bool track=m_selections && biome<39 && seed<=UINT32_MAX &&
            x>=INT32_MIN+128 && x<=INT32_MAX-128 && z>=INT32_MIN+128 && z<=INT32_MAX-128;
        if(track)m_selections->reserve(m_selections->size()+1);
        const uint32_t handle=uint32_t(int(type)*36+variant);
        const bool ok=defer_air ? Gpu_Tree_Overlay::append_deferred_coverage(m_words,*m_trees,
            Gpu_Tree_Overlay::tree_coverage_catalog()[handle],handle,int64_t(x)-ox,int64_t(y)-oy,int64_t(z)-oz) :
            Gpu_Tree_Overlay::append(m_words,*m_trees,tree,handle,
            int64_t(x)-ox,int64_t(y)-oy,int64_t(z)-oz,[&](int lx,int ly,int lz){
                return allow(int(ox+lx),int(oy+ly),int(oz+lz));
            },defer_air);
        if(ok&&track&&m_trees->size()>before)m_selections->push_back({uint32_t(before),x,z,uint32_t(seed),biome});
        return ok;
    }
    // Caller establishes the shared terrain/slope/village gates. GPU owns the
    // final species, density, anchor and variant decision; rejection is a no-op.
    bool capture_tree_candidate(uint32_t biome,int x,int y,int z,uint32_t seed){
        using namespace Gpu_Tree_Overlay;
        const auto r=species_rule(biome);
        if(!m_trees||!m_selections||r.species==no_tree||catalog_words().empty()||
           m_native_candidates>=32||m_native_candidate_words>=32768||
           x<INT32_MIN+128||x>INT32_MAX-128||z<INT32_MIN+128||z>INT32_MAX-128)return false;
        const auto before=m_trees->size(),wordBefore=m_words.size();m_selections->reserve(m_selections->size()+1);
        const auto& coverage=biome==8?savanna_coverage():species_coverage_catalog()[r.species];
        if(!append_variant_candidate(m_words,*m_trees,coverage,r.species,
            int64_t(x)-int64_t(m_x)*32,int64_t(y)-int64_t(m_y)*32,int64_t(z)-int64_t(m_z)*32))return false;
        const size_t added=m_words.size()-wordBefore;
        if(added>32768-m_native_candidate_words){m_words.resize(wordBefore);m_trees->resize(before);return false;}
        if(m_trees->size()>before){m_selections->push_back({uint32_t(before),x,z,seed,candidate_flag|biome});++m_native_candidates;m_native_candidate_words+=added;}
        return true;
    }
    bool capture_structure(const Structure_Instance& inst){
        if(!m_trees||inst.custom_template||inst.rotation>3u||
            static_cast<uint32_t>(inst.template_id)>=Gpu_Tree_Overlay::structure_templates/4)return false;
        const int64_t ox=int64_t(m_x)*32,oy=int64_t(m_y)*32,oz=int64_t(m_z)*32;
        const int minY=int(std::max<int64_t>(0,int64_t(Config::world_min_y)-oy));
        const int maxY=int(std::min<int64_t>(31,int64_t(Config::world_max_y)-oy));
        return Gpu_Tree_Overlay::append_structure(m_words,*m_trees,Gpu_Tree_Overlay::catalog_words(),
            Gpu_Tree_Overlay::templates+static_cast<uint32_t>(inst.template_id)*4+inst.rotation,
            int64_t(inst.entrance_world_x)-ox,int64_t(inst.base_world_y)-oy,int64_t(inst.entrance_world_z)-oz,minY,maxY);
    }
    int get_chunk_x() const { return m_x; }
    int get_chunk_y() const { return m_y; }
    int get_chunk_z() const { return m_z; }
    void set_block(int x, int y, int z, Block_Types type) {
        if (inside(x,y,z)) m_words.push_back((address(x,y,z) << 16u) | uint32_t(static_cast<Block_ID>(type)));
    }
    void set_meta(int x, int y, int z, uint8_t value) {
        if (inside(x,y,z)) m_words.push_back(0x80000000u | (address(x,y,z) << 16u) | value);
    }
    const std::vector<uint32_t>& words() const { return m_words; }
    template<class BlockWrite, class MetaWrite>
    void replay(BlockWrite&& block, MetaWrite&& meta) const {
        for (uint32_t word : m_words) {
            const int x = int((word >> 16u) & 31u);
            const int z = int((word >> 21u) & 31u);
            const int y = int((word >> 26u) & 31u);
            if (word & 0x80000000u) meta(x,y,z,static_cast<uint8_t>(word));
            else block(x,y,z,static_cast<Block_Types>(word & 0xffffu));
        }
    }
};

// Shared placement order for CPU and GPU finalization.
template<class Generator>
void collect_generation_overlay(const Generator& generator, Generation_Overlay& overlay) {
    const int x0 = overlay.get_chunk_x() * Config::chunk_size;
    const int z0 = overlay.get_chunk_z() * Config::chunk_size;
    if (generator.may_decorate_chunk(overlay.get_chunk_x(), overlay.get_chunk_y(), overlay.get_chunk_z())) {
        for (int x = -Config::decoration_overhang; x < Config::chunk_size + Config::decoration_overhang; ++x)
            for (int z = -Config::decoration_overhang; z < Config::chunk_size + Config::decoration_overhang; ++z)
                generator.decorate_column(overlay, x0 + x, z0 + z);
    }
    generator.place_villages_in_chunk(overlay);
}
