#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <vector>

namespace OptiCraft::Vulkan {
// Input is the existing dropped-item triangle list. Compact only exact
// emit_quad patterns, preserve arbitrary triangles and all vertex attributes.
// No hash table or global weld; stable order preserves depth ties.
template<class Vertex> struct ItemQuadIndexPlan {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    size_t compactedQuads=0;
    static bool equal(const Vertex& a,const Vertex& b) noexcept {
        auto bits=[](const Vertex& v){return std::array<uint32_t,9>{
            std::bit_cast<uint32_t>(v.x),std::bit_cast<uint32_t>(v.y),std::bit_cast<uint32_t>(v.z),
            std::bit_cast<uint32_t>(v.u),std::bit_cast<uint32_t>(v.v),std::bit_cast<uint32_t>(v.nx),
            std::bit_cast<uint32_t>(v.ny),std::bit_cast<uint32_t>(v.nz),std::bit_cast<uint32_t>(v.light)};};
        return bits(a)==bits(b);
    }
    // Source must not alias this plan's storage.
    void build(std::span<const Vertex> source){
        if(source.size()>UINT32_MAX || source.size()%3)throw std::invalid_argument("item triangle list");
        vertices.clear();indices.clear();compactedQuads=0;
        vertices.reserve(source.size());indices.reserve(source.size());
        for(size_t i=0;i<source.size();){
            const auto base=static_cast<uint32_t>(vertices.size());
            if(source.size()-i>=6 && equal(source[i],source[i+3]) && equal(source[i+2],source[i+4])){
                for(size_t j:{0u,1u,2u,5u})vertices.push_back(source[i+j]);
                for(uint32_t j:{0u,1u,2u,0u,2u,3u})indices.push_back(base+j);
                ++compactedQuads;i+=6;
            }else{
                for(uint32_t j=0;j<3;++j){vertices.push_back(source[i+j]);indices.push_back(base+j);}i+=3;
            }
        }
    }
    bool savesBytes() const noexcept {return compactedQuads>0 && uint64_t(compactedQuads)*2*sizeof(Vertex)>uint64_t(indices.size())*sizeof(uint32_t);}
};
}
