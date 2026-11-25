#pragma once
#include "world/generation/Gpu_Template_Overlay.h"
#include <utility>
#include <cstdlib>

namespace Gpu_Tree_Selection {
using namespace Gpu_Template_Overlay;
constexpr uint32_t tree_count=16*36, catalog_count=tree_count+5, max_candidates=4096;
static_assert(Tree_Pack::k_species_count==16 && Tree_Pack::k_variants_per_species==36);
static_assert(static_cast<int>(Structure_Template_Id::Count)==5);
static_assert(int(Tree_Type::Oak)==0 && int(Tree_Type::Palm)==1 && int(Tree_Type::Acacia)==2 &&
    int(Tree_Type::Birch)==3 && int(Tree_Type::Spruce)==4 && int(Tree_Type::Jungle)==5 &&
    int(Tree_Type::Slimewood)==6 && int(Tree_Type::Beech)==7 && int(Tree_Type::Redwood)==8 &&
    int(Tree_Type::Mangrove)==9 && int(Tree_Type::Willow)==10 && int(Tree_Type::Baobab)==13 &&
    int(Tree_Type::Cherry)==14 && int(Tree_Type::Aspen)==15);
// Stable handles species*36+variant, then built-in structures. Immutable for
// the lifetime of a GPU upload; retain old allocation until its fence retires.
class Catalog {
    Packet packet_;
    std::vector<Instance> descriptors_;
public:
    bool build(const Tree_Pack& trees,const Structure_Pack& structures) {
        Packet next;
        for(int s=0;s<16;++s)for(int v=0;v<36;++v){
            uint32_t h;
            if(!next.add_tree(trees.template_at(Tree_Type(s),v),h)||h!=uint32_t(s*36+v))return false;
        }
        for(int i=0;i<5;++i){uint32_t h;auto t=structures.get(Structure_Template_Id(i));
            if(!t||!next.add_structure(*t,h)||h!=tree_count+uint32_t(i))return false;
        }
        std::vector<Instance> d;d.reserve(catalog_count);
        for(const auto& r:next.ranges())d.push_back({0,0,0,0,r.begin,r.end,
            uint32_t(d.size()<tree_count?Predicate::Air:Predicate::Always),0,
            r.lo[0],r.lo[1],r.lo[2],0,r.hi[0],r.hi[1],r.hi[2],0});
        packet_=std::move(next);descriptors_=std::move(d);return true;
    }
    std::span<const Record> records()const noexcept{return packet_.records();}
    std::span<const Instance> descriptors()const noexcept{return descriptors_;}
    bool ready()const noexcept{return descriptors_.size()==catalog_count;}
};
// In original decoration traversal order. `eligible` means all shared CPU
// gates (dimension, quick height, actual surface, village exclusion) passed.
// `gentle` is slope<=4; `palm_roll` is the original second RNG roll==0.
// Keeping the RNG result explicit preserves stdlib-specific distribution.
struct Candidate {int32_t x,y,z;uint32_t biome,eligible,gentle,palm_roll,reserved;};
struct SelectPush {uint32_t count,seed;};
struct CompactPush {uint32_t count;int32_t origin_x,origin_y,origin_z;};
// Compaction writes a zero-range sentinel unless all 128 slots are occupied.
// Overlay may use instanceCount=128 without reading back count. On failure the
// first slot is the sentinel; result must still be checked before publication.
struct Result {uint32_t count,overflow;};
static_assert(sizeof(Candidate)==32 && sizeof(SelectPush)==8 && sizeof(CompactPush)==16 && sizeof(Result)==8);
inline bool prepare(std::span<const Candidate> input,uint32_t seed,int cx,int cy,int cz,SelectPush& select,CompactPush& compact) noexcept {
    if(input.size()>max_candidates)return false;
    uint32_t lo,hi;if(!y_interval(cy,lo,hi))return false;
    int64_t ox=int64_t(cx)*32,oy=int64_t(cy)*32,oz=int64_t(cz)*32;
    if(ox<INT32_MIN+128ll||ox>INT32_MAX-128ll||oz<INT32_MIN+128ll||oz>INT32_MAX-128ll)return false;
    for(auto c:input)if(c.biome>=39||c.eligible>1||c.gentle>1||c.palm_roll>1||c.reserved||
        std::abs(int64_t(c.x)-ox)>64||std::abs(int64_t(c.z)-oz)>64||c.y<Config::world_min_y||c.y>Config::world_max_y)return false;
    select={uint32_t(input.size()),seed};compact={uint32_t(input.size()),int32_t(ox),int32_t(oy),int32_t(oz)};return true;
}
constexpr uint32_t no_tree=0xffffffffu;
constexpr uint32_t groups(uint32_t n)noexcept{return (n+63)/64;}
} // namespace Gpu_Tree_Selection
