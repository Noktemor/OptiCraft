#pragma once
#include "world/generation/Gpu_Tree_Overlay_Data.h"
#include "world/generation/Tree_Pack.h"
#include "world/generation/Structure_Pack.h"
#include <tuple>

namespace Gpu_Tree_Overlay {
static_assert(static_cast<uint32_t>(Structure_Template_Id::Count)*4==structure_templates);
// Compact two-word tree records and three-word rotated structure records.
// Stable coordinate sort preserves duplicate writes at the same voxel.
inline std::vector<uint32_t> build_catalog(const Tree_Pack& pack,const Structure_Pack& structures) {
    if(!pack.valid())return {};
    std::vector<uint32_t> out(total_templates*2);
    uint32_t cursor=0;
    for(uint32_t h=0;h<templates;++h){
        auto view=pack.template_at(Tree_Type(h/36),int(h%36));
        if(view.size()>max_records-cursor)return {};
        std::vector<Tree_Pack::Block> sorted(view.begin(),view.end());
        std::stable_sort(sorted.begin(),sorted.end(),[](auto a,auto b){return std::tuple{a.y,a.z,a.x}<std::tuple{b.y,b.z,b.x};});
        out[h*2]=uint32_t(out.size());
        for(auto b:sorted){
            if(static_cast<Block_ID>(b.type)==65535u)return {};
            out.push_back(uint32_t(uint16_t(b.x))|(uint32_t(uint16_t(b.y))<<16u));
            out.push_back(uint32_t(uint16_t(b.z))|(uint32_t(static_cast<Block_ID>(b.type))<<16u));
        }
        cursor+=uint32_t(sorted.size());out[h*2+1]=uint32_t(out.size());
    }
    if(!cursor)return {};
    if(structures.ready())for(uint32_t kind=0;kind<structure_templates/4;++kind){
        auto t=structures.get(Structure_Template_Id(kind));if(!t)continue;
        for(uint32_t rotation=0;rotation<4;++rotation){
            const uint32_t h=templates+kind*4+rotation;
            if(t->voxels.size()>max_records-cursor)continue; // trees and earlier templates remain valid
            auto sorted=t->voxels;
            for(auto& b:sorted){
                int x,z;Structure_Pack::rotate_xz(int(b.x)-t->entrance_x,int(b.z)-t->entrance_z,uint8_t(rotation),x,z);
                b.x=int16_t(x);b.z=int16_t(z);
            }
            if(std::any_of(sorted.begin(),sorted.end(),[](auto b){return static_cast<Block_ID>(b.type)==65535u;}))continue;
            std::stable_sort(sorted.begin(),sorted.end(),[](auto a,auto b){return std::tuple{a.y,a.z,a.x}<std::tuple{b.y,b.z,b.x};});
            out[h*2]=uint32_t(out.size());
            for(auto b:sorted){
                out.push_back(uint32_t(uint16_t(b.x))|(uint32_t(uint16_t(b.y))<<16u));
                out.push_back(uint32_t(uint16_t(b.z))|(uint32_t(static_cast<Block_ID>(b.type))<<16u));
                const bool writeMeta=b.type!=Block_Types::Air&&(b.meta!=0||(b.flags&1u));
                const uint32_t meta=(b.flags&1u)?((b.meta&~Block_Meta::direction_mask)|((b.meta+rotation)&Block_Meta::direction_mask)):b.meta;
                out.push_back(writeMeta?(256u|meta):0u);
            }
            cursor+=uint32_t(sorted.size());out[h*2+1]=uint32_t(out.size());
        }
    }
    return out;
}
inline std::span<const uint32_t> catalog_words(){
    static const auto words=build_catalog(Tree_Pack::instance(),Structure_Pack::instance());return words;
}
}
