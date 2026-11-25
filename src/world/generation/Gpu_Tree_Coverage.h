#pragma once
#include "world/generation/Gpu_Tree_Overlay_Data.h"
#include "world/generation/Tree_Pack.h"

// Immutable geometry index for deferred air placement; one entry per packed
// GPU owner, for each of four horizontal alignments. No world state is cached.
namespace Gpu_Tree_Overlay {
struct CoverageGroup { int16_t y,z,x_group; uint16_t first_id; uint8_t mask; bool multiple; };
struct TreeCoverage { std::array<std::vector<CoverageGroup>,4> alignments; bool ready=false; };
constexpr int floor_group(int x) noexcept {return x/4-int(x%4<0);}
template<class Block>
TreeCoverage build_coverage(std::span<const Block> tree) {
    TreeCoverage out;
    if(tree.empty())return out;
    for(const auto& b:tree)if(uint16_t(b.type)>=0x4000u)return out;
    for(int phase=0;phase<4;++phase){
        auto& groups=out.alignments[phase];groups.reserve(tree.size());
        for(const auto& b:tree){
            const int x=int(b.x)+phase,g=floor_group(x);
            groups.push_back({b.y,b.z,int16_t(g),uint16_t(b.type),uint8_t(1u<<(x-4*g)),false});
        }
        auto key=[](const CoverageGroup& g){return std::tuple{g.y,g.z,g.x_group};};
        std::stable_sort(groups.begin(),groups.end(),[&](auto a,auto b){return key(a)<key(b);});
        size_t n=0;
        for(const auto g:groups){
            if(n&&key(groups[n-1])==key(g)){groups[n-1].mask|=g.mask;groups[n-1].multiple=true;}
            else groups[n++]=g;
        }
        groups.resize(n);groups.shrink_to_fit();
    }
    out.ready=true;return out;
}
inline const std::array<TreeCoverage,templates>& tree_coverage_catalog(){
    static const auto index=[] {
        std::array<TreeCoverage,templates> result;
        const auto& pack=Tree_Pack::instance();
        if(pack.valid())for(uint32_t h=0;h<templates;++h)
            result[h]=build_coverage(pack.template_at(Tree_Type(h/36),int(h%36)));
        return result;
    }();
    return index;
}
// Same per-owner words as append(...,defer_air=true). CSR preserves inter-instance
// ordering independently of the order in which distinct owners are emitted.
inline bool append_deferred_coverage(std::vector<uint32_t>& words,std::vector<Instance>& instances,
        const TreeCoverage& coverage,uint32_t handle,int64_t x,int64_t y,int64_t z) {
    if(!coverage.ready||instances.size()>=max_instances||handle>=templates||
       x < -32767||x>32767||y < -32767||y>32767||z < -32767||z>32767)return false;
    const int gx=floor_group(int(x)),phase=int(x)-4*gx;
    const auto& groups=coverage.alignments[phase];
    const auto begin=std::lower_bound(groups.begin(),groups.end(),-y,[](const CoverageGroup& a,int64_t v){return a.y<v;});
    const auto end=std::lower_bound(begin,groups.end(),32-y,[](const CoverageGroup& a,int64_t v){return a.y<v;});
    const size_t required=words.size()+size_t(end-begin);
    if(required>words.capacity())words.reserve(std::max(required,words.capacity()+words.capacity()/2));
    if(begin!=end)instances.reserve(instances.size()+1);
    const uint32_t index=uint32_t(instances.size());bool used=false;
    for(auto it=begin;it!=end;++it){
        const int64_t gz=z+it->z,xx=gx+it->x_group;
        if(gz<0||gz>=32||xx<0||xx>=8)continue;
        const uint32_t group=uint32_t(y+it->y)*256u+uint32_t(gz)*8u+uint32_t(xx);
        if(it->multiple){words.push_back(encode(group,index,it->mask)|air_flag);used=true;}
        else {
            const uint32_t lane=(it->mask&1u)?0u:(it->mask&2u)?1u:(it->mask&4u)?2u:3u;
            words.push_back(encode_air_write(group*4u+lane,it->first_id));
        }
    }
    if(used)instances.push_back({int32_t(x),int32_t(y),int32_t(z),handle});
    return true;
}
// Union of the geometry of every variant of a species. Every marker is
// expanded through the GPU-selected handle, including single-lane groups.
inline const std::array<TreeCoverage,templates/36>& species_coverage_catalog(){
    static const auto unions=[] {
        std::array<TreeCoverage,templates/36> result;
        const auto& index=tree_coverage_catalog();
        for(uint32_t s=0;s<result.size();++s){
            auto& dst=result[s];
            bool ready=true;for(uint32_t v=0;v<36;++v)ready&=index[s*36+v].ready;
            if(!ready)continue;
            for(int phase=0;phase<4;++phase){
                auto& groups=dst.alignments[phase];
                for(uint32_t v=0;v<36;++v){const auto& src=index[s*36+v].alignments[phase];groups.insert(groups.end(),src.begin(),src.end());}
                auto key=[](const CoverageGroup& g){return std::tuple{g.y,g.z,g.x_group};};
                std::sort(groups.begin(),groups.end(),[&](auto a,auto b){return key(a)<key(b);});
                size_t n=0;for(auto g:groups){if(n&&key(groups[n-1])==key(g))groups[n-1].mask|=g.mask;else groups[n++]=g;}
                groups.resize(n);groups.shrink_to_fit();
            }
            dst.ready=true;
        }
        return result;
    }();return unions;
}
// Savanna can choose either acacia or baobab. Union both immutable species indices.
inline const TreeCoverage& savanna_coverage(){
    static const auto merged=[] {
        const auto& species=species_coverage_catalog();TreeCoverage out;
        if(!species[2].ready||!species[13].ready)return out;
        for(int phase=0;phase<4;++phase){
            auto& g=out.alignments[phase];g=species[2].alignments[phase];
            const auto& b=species[13].alignments[phase];g.insert(g.end(),b.begin(),b.end());
            auto key=[](const CoverageGroup& v){return std::tuple{v.y,v.z,v.x_group};};
            std::sort(g.begin(),g.end(),[&](auto a,auto b){return key(a)<key(b);});
            size_t n=0;for(auto v:g){if(n&&key(g[n-1])==key(v))g[n-1].mask|=v.mask;else g[n++]=v;}
            g.resize(n);g.shrink_to_fit();
        }out.ready=true;return out;
    }();return merged;
}
inline bool append_variant_candidate(std::vector<uint32_t>& words,std::vector<Instance>& instances,
        const TreeCoverage& coverage,uint32_t species,int64_t x,int64_t y,int64_t z){
    if(!coverage.ready||species>=templates/36||instances.size()>=max_instances||
        x < -32767||x>32767||y < -32767||y>32767||z < -32767||z>32767)return false;
    const int gx=floor_group(int(x)),phase=int(x)-4*gx;
    const auto& groups=coverage.alignments[phase];
    const auto begin=std::lower_bound(groups.begin(),groups.end(),-y,[](const CoverageGroup& a,int64_t v){return a.y<v;});
    const auto end=std::lower_bound(begin,groups.end(),32-y,[](const CoverageGroup& a,int64_t v){return a.y<v;});
    if(begin==end)return true;
    const size_t required=words.size()+size_t(end-begin);
    if(required>words.capacity())words.reserve(std::max(required,words.capacity()+words.capacity()/2));
    instances.reserve(instances.size()+1);
    const auto idx=uint32_t(instances.size());bool used=false;
    for(auto it=begin;it!=end;++it){
        const int64_t gz=z+it->z,xx=gx+it->x_group;
        if(gz<0||gz>=32||xx<0||xx>=8)continue;
        const uint32_t group=uint32_t(y+it->y)*256u+uint32_t(gz)*8u+uint32_t(xx);
        words.push_back(encode(group,idx,it->mask)|air_flag);used=true;
    }
    if(used)instances.push_back({int32_t(x),int32_t(y),int32_t(z),species*36});
    return true;
}

}
