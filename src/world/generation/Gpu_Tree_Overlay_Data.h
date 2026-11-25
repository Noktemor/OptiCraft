#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <span>
#include <vector>
#include <limits>
#include <algorithm>
#include <tuple>
#include "world/generation/Generation_Sky.h"

// Ordered tree commands share the existing 4-voxel CSR owners. The bit 15
// tag is reserved in metadata commands (ordinary metadata only uses 8 bits).
// Each marker specifies one instance and clipped candidate/authorized lanes.
// Conditional markers test immutable base air during ordered GPU expansion.
namespace Gpu_Tree_Overlay {
constexpr uint32_t templates=576, structure_templates=5*4, total_templates=templates+structure_templates;
constexpr uint32_t max_instances=128, max_records=1u<<20;
constexpr uint32_t tag=0x80008000u;
// Stage137: conditional markers and standalone writes test the immutable
// base ID, never the result of earlier decoration commands in this owner.
constexpr uint32_t air_flag=0x2000u, air_write_tag=0x80004000u;
constexpr bool air_write(uint32_t w) noexcept {return (w&0x8000c000u)==air_write_tag;}
constexpr uint32_t air_id(uint32_t w) noexcept {return w&0x3fffu;}
constexpr uint32_t encode_air_write(uint32_t voxel,uint32_t id) noexcept {return air_write_tag|(voxel<<16u)|id;}
constexpr uint32_t instance_offset=131072u+uint32_t(Generation_Sky::seed_input_words);
constexpr uint32_t range_offset=instance_offset+max_instances*4;
constexpr uint32_t record_offset=range_offset+total_templates*2;
struct Instance {int32_t x,y,z;uint32_t handle;};
static_assert(sizeof(Instance)==16);
// Legacy entries compare CPU selection. Variant-only entries own a union
// of all variants of the CPU-approved species; GPU selects the actual handle.
// One entry per captured surface tree, excluding legacy trees and buildings.
struct Selection {uint32_t instance; int32_t x,z; uint32_t seed,biome,result=0xffffffffu;};
static_assert(sizeof(Selection)==24);
constexpr uint32_t selection_mode=0x80000002u, variant_only=0x80000000u, candidate_flag=0x80000100u, no_tree=0xffffffffu;
struct SpeciesRule {uint32_t species;int radius;uint32_t density;};
constexpr SpeciesRule species_rule(uint32_t biome) noexcept {
    switch(biome){
    case 4:return {0,3,55};case 5:return {3,3,40};case 28:return {15,4,48};
    case 27:return {7,4,52};case 26:return {14,4,40};case 23:return {8,5,35};
    case 6:case 33:return {4,4,60};case 7:return {5,4,50};case 24:return {10,5,34};
    case 25:return {9,4,58};case 8:return {2,4,27};case 17:return {6,0,100};
    default:return {no_tree,0,0};}
}
constexpr bool candidate_selection(uint32_t flags) noexcept {
    return (flags&0xffffff00u)==candidate_flag && species_rule(flags&255u).species!=no_tree;
}
constexpr bool independent_selection(uint32_t flags) noexcept {return flags==variant_only||candidate_selection(flags);}
constexpr uint64_t candidate_hash(int x,int z,uint32_t seed,uint32_t salt) noexcept {
    uint64_t h=uint64_t(seed)^(uint64_t(int64_t(x))*0x9e3779b185ebca87ull)^(uint64_t(int64_t(z))*0xc2b2ae3d27d4eb4full)^(uint64_t(salt)*0x165667b19e3779f9ull);
    h=(h^(h>>29))*0xbf58476d1ce4e5b9ull;return h^(h>>32);
}
constexpr uint32_t fallback_species(int x,int z,uint32_t seed,uint32_t biome) noexcept {
    auto r=species_rule(biome);if(r.species==no_tree)return no_tree;
    if(biome==8 && candidate_hash(x,z,seed,0xba0b)%4==0){r.species=13;r.radius=5;}
    if(r.species==6){
        auto fd=[](int a,int d){return a/d-int(a%d<0);};
        const int cx=fd(x,60),cz=fd(z,60);const auto h=candidate_hash(cx,cz,seed,0x5c1f);
        return (h>>32)%100<40 && x==cx*60+25+int(h%11) && z==cz*60+25+int((h>>16)%11)?6u:no_tree;
    }
    if(candidate_hash(x,z,seed,0x7de5)%100>=r.density)return no_tree;
    const auto h=candidate_hash(x,z,seed,9);
    for(int dz=-r.radius;dz<=r.radius;++dz)for(int dx=-r.radius;dx<=r.radius;++dx)
        if((dx||dz)&&candidate_hash(x+dx,z+dz,seed,9)>=h)return no_tree;
    return r.species;
}
// Same integer rule as Tree_Pack::variant_at, used only on catalog fallback.
constexpr uint32_t fallback_variant(int32_t x,int32_t z,uint32_t species,uint32_t seed) noexcept {
    auto fd=[](int a,int d){return a/d-int(a%d<0);};
    auto mix=[](uint64_t h){h=(h^(h>>30))*0xbf58476d1ce4e5b9ull;h=(h^(h>>27))*0x94d049bb133111ebull;return h^(h>>31);};
    const int cx=fd(x,16),cz=fd(z,16),tx=fd(cx,6),tz=fd(cz,6);
    auto h=mix(uint64_t(seed)^(uint64_t(species)<<48));
    h=mix(h^(uint64_t(int64_t(tx))*0x9e3779b185ebca87ull));
    h=mix(h^(uint64_t(int64_t(tz))*0xc2b2ae3d27d4eb4full));
    return (uint32_t((cz-tz*6)*6+cx-tx*6)+uint32_t(h%36))%36;
}
inline bool valid_selections(std::span<const Selection> selections,std::span<const Instance> instances) noexcept {
    if(selections.size()>instances.size())return false;
    uint32_t previous=0;bool first=true;
    for(auto s:selections){
        if(s.instance>=instances.size()||(!first&&s.instance<=previous)||instances[s.instance].handle>=templates||
           (s.biome>=39 && !independent_selection(s.biome))||
           (candidate_selection(s.biome) && instances[s.instance].handle!=species_rule(s.biome&255u).species*36u)||
           (s.biome==variant_only && instances[s.instance].handle%36!=0)||s.result!=0xffffffffu||s.x<INT32_MIN+128||s.x>INT32_MAX-128||
           s.z<INT32_MIN+128||s.z>INT32_MAX-128)return false;
        previous=s.instance;first=false;
    }
    return true;
}

constexpr bool marker(uint32_t w) noexcept {return (w&tag)==tag;}
constexpr uint32_t index(uint32_t w) noexcept {return w&127u;}
constexpr uint32_t lanes(uint32_t w) noexcept {return (w>>8u)&15u;}
constexpr uint32_t encode(uint32_t group,uint32_t instance,uint32_t mask) noexcept {
    return tag|(group<<18u)|(mask<<8u)|instance;
}
inline bool valid(std::span<const Instance> instances) noexcept {
    if(instances.size()>max_instances)return false;
    for(auto a:instances)if(a.handle>=total_templates||a.x < -32767||a.x>32767||a.y < -32767||a.y>32767||a.z < -32767||a.z>32767)return false;
    return true;
}
constexpr bool valid_marker(uint32_t w,size_t count) noexcept {
    return marker(w) && !(w&0x00035080u) && lanes(w)!=0u && index(w)<count;
}
// Blocks expose signed local coordinates and a 16-bit ID. Without defer_air,
// mask computation uses the original CPU predicate. With defer_air, masks
// describe only geometry coverage and GPU evaluates the air predicate.
// Duplicate coordinates are deliberately retained by the catalog; the mask
// authorizes a position, not a particular write. Call only with a predicate
// that depends on the immutable procedural base, never the mutable overlay.
template<class Block,class Predicate>
bool append(std::vector<uint32_t>& words,std::vector<Instance>& instances,
            std::span<const Block> tree,uint32_t handle,int64_t x,int64_t y,int64_t z,Predicate&& allow,bool defer_air=false) {
    if(tree.empty()||instances.size()>=max_instances||handle>=templates||
       x < -32767||x>32767||y < -32767||y>32767||z < -32767||z>32767)return false;
    std::array<uint8_t,8192> masks{};
    std::array<uint32_t,8192> first; // initialized on first authorized write
    std::vector<uint16_t> touched; touched.reserve(std::min<size_t>(tree.size(),8192));
    for(auto b:tree){
        if(static_cast<uint16_t>(b.type)==65535u || (defer_air && static_cast<uint16_t>(b.type)>=0x4000u))return false;
        const int64_t lx=x+b.x,ly=y+b.y,lz=z+b.z;
        if(lx<0||lx>=32||ly<0||ly>=32||lz<0||lz>=32)continue;
        const uint32_t voxel=uint32_t(ly)*1024+uint32_t(lz)*32+uint32_t(lx);
        const uint32_t group=voxel/4,bit=1u<<(voxel%4);
        if(!defer_air && !(masks[group]&bit) && !allow(int(lx),int(ly),int(lz)))continue;
        if(!masks[group]){touched.push_back(uint16_t(group));first[group]=defer_air?encode_air_write(voxel,uint16_t(b.type)):(voxel<<16)|uint16_t(b.type);}
        else masks[group]|=16u; // multiple writes (including duplicate coordinates)
        masks[group]|=uint8_t(bit);
    }
    if(touched.empty())return true;
    // Strong exception guarantee for two correlated vectors.
    const bool useTemplate=std::any_of(touched.begin(),touched.end(),[&](auto g){return (masks[g]&16u)!=0;});
    const size_t required=words.size()+touched.size();
    if(required>words.capacity())words.reserve(std::max(required,words.capacity()+words.capacity()/2));
    uint32_t i=uint32_t(instances.size());
    if(useTemplate){instances.reserve(instances.size()+1);instances.push_back({int32_t(x),int32_t(y),int32_t(z),handle});}
    // Avoid binary searches for isolated writes; retain compact normal ops.
    for(auto g:touched)words.push_back((masks[g]&16u)?(encode(g,i,masks[g]&15u)|(defer_air?air_flag:0u)):first[g]);
    return true;
}
// Catalog ranges are offsets in words, relative to the range table start.
// Trees use two words per record; rotated buildings add one metadata word
// (bit 8 = explicit write, including value zero). Offsets are independent of
// the tree record count and permit empty/missing structure ranges.
struct CatalogRange {uint32_t begin,end,stride;};
inline bool catalog_range(std::span<const uint32_t> catalog,uint32_t handle,CatalogRange& r) noexcept {
    if(handle>=total_templates||catalog.size()<total_templates*2u)return false;
    r={catalog[handle*2],catalog[handle*2+1],handle<templates?2u:3u};
    return r.begin>=total_templates*2u && r.begin<r.end && r.end<=catalog.size() && (r.end-r.begin)%r.stride==0;
}
inline auto catalog_key(std::span<const uint32_t> catalog,uint32_t word) noexcept {
    auto sx=[](uint32_t n){return int32_t(n&65535u)-((n&32768u)?65536:0);};
    const auto p=catalog[word],q=catalog[word+1];return std::tuple{sx(p>>16u),sx(q),sx(p)};
}
inline uint32_t catalog_lower(std::span<const uint32_t> catalog,CatalogRange r,std::tuple<int32_t,int32_t,int32_t> key) noexcept {
    uint32_t lo=0,hi=(r.end-r.begin)/r.stride;
    while(lo<hi){const auto mid=lo+(hi-lo)/2u;if(catalog_key(catalog,r.begin+mid*r.stride)<key)lo=mid+1;else hi=mid;}
    return r.begin+lo*r.stride;
}
// Capture the clipped group mask from the immutable rotated catalog. Only the
// intersecting y interval is traversed; no per-instance rotation or metadata
// expansion is necessary. A zero-meta directional block still gets its write.
inline bool append_structure(std::vector<uint32_t>& words,std::vector<Instance>& instances,
                             std::span<const uint32_t> catalog,uint32_t handle,int64_t x,int64_t y,int64_t z,
                             int minY,int maxY) {
    CatalogRange r{};
    if(handle<templates||!catalog_range(catalog,handle,r)||instances.size()>=max_instances||
        x < -32767||x>32767||y < -32767||y>32767||z < -32767||z>32767)return false;
    if(minY<0||maxY>31)return false;
    if(minY>maxY)return true;
    std::array<uint8_t,8192> masks{};
    std::vector<uint16_t> touched;
    const auto first=catalog_lower(catalog,r,{int32_t(minY-y),INT32_MIN,INT32_MIN});
    for(uint32_t word=first;word<r.end;word+=r.stride){
        const auto [ry,rz,rx]=catalog_key(catalog,word);
        const int64_t ly=y+ry,lz=z+rz,lx=x+rx;
        if(ly>maxY)break;
        if(lx<0||lx>=32||lz<0||lz>=32)continue;
        const uint32_t voxel=uint32_t(ly)*1024+uint32_t(lz)*32+uint32_t(lx),group=voxel/4;
        if(!masks[group])touched.push_back(uint16_t(group));
        masks[group]|=uint8_t(1u<<(voxel%4));
    }
    if(touched.empty())return true;
    words.reserve(words.size()+touched.size());instances.reserve(instances.size()+1);
    const uint32_t i=uint32_t(instances.size());instances.push_back({int32_t(x),int32_t(y),int32_t(z),handle});
    for(auto g:touched)words.push_back(encode(g,i,masks[g]));
    return true;
}
// CPU fallback: same clipped lanes, ordering and explicit metadata semantics.
inline bool expand_marker(uint32_t op,std::span<const Instance> instances,
                          std::span<const uint32_t> catalog,std::vector<uint32_t>& out,size_t limit) {
    if(!valid_marker(op,instances.size()))return false;
    const auto a=instances[index(op)];if(!valid(std::span<const Instance>(&a,1)))return false;
    CatalogRange r{};if(!catalog_range(catalog,a.handle,r))return false;
    const bool conditional=(op&air_flag)!=0;
    if(conditional && a.handle>=templates)return false;
    const uint32_t group=(op>>18u)&8191u,v=group*4u;
    const int32_t x=int32_t(v&31u)-a.x,y=int32_t(v>>10u)-a.y,z=int32_t((v>>5u)&31u)-a.z;
    const auto lo=catalog_lower(catalog,r,{y,z,x});
    for(uint32_t i=lo;i<r.end;i+=r.stride){
        const auto [ry,rz,rx]=catalog_key(catalog,i);if(ry!=y||rz!=z||rx>=x+4)break;
        if(rx<x)return false;
        const uint32_t lane=uint32_t(rx-x);if(!(lanes(op)&(1u<<lane)))continue;
        const uint32_t id=catalog[i+1]>>16u;
        const uint32_t meta=r.stride==3?catalog[i+2]:0u;
        if(out.size()>=limit||id==65535u||meta>511u)return false;
        if(conditional && id>=0x4000u)return false;
        out.push_back(conditional?encode_air_write(v+lane,id):((v+lane)<<16u)|id);
        if(meta&256u){if(out.size()>=limit)return false;out.push_back(0x80000000u|((v+lane)<<16u)|(meta&255u));}
    }
    return true;
}

}
