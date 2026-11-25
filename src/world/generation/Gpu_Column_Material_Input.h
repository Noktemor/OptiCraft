#pragma once
#include "world/generation/Gpu_Layer_Data.h"
#include "world/generation/Gpu_Cave_Data.h"
#include "world/generation/Gpu_River_Data.h"
#include "world/generation/Gpu_Worldgen_Data.h"
#include <algorithm>
#include <array>
#include <optional>
#include <span>

// Stage122-124: one column's existing material ABI, produced on GPU rather
// than repacking the layer/river/cave results on the CPU. These are inputs,
// not a publication certificate. All sources must belong to the same world.
struct Gpu_Column_Material_Input {
    static constexpr uint32_t max_levels=Gpu_Base_Region::levels_reserved;
    static constexpr uint32_t header_words=Gpu_Base_Region::levels_reserved+Gpu_Base_Region::cave_offset;
    static constexpr uint32_t level_words=Gpu_Base_Region::cave_words;
    static constexpr uint32_t init_groups=Gpu_Base_Region::columns/64;
    struct Push {
        uint32_t count,flags;
        int32_t sea;
        uint32_t water_floor;
        int32_t ys[max_levels];
    };
    struct CavePush { uint32_t column,columns,count; };
    struct Region {int32_t x,z;};
    static constexpr uint32_t words(uint32_t n) {return header_words+n*level_words;}
    static constexpr uint32_t output_words(uint32_t n) {return n*(Gpu_Base_Region::voxels/2+1);}
    static bool make(int32_t cx,int32_t cz,std::span<const int32_t> levels,int32_t sea,
                     uint32_t water,uint32_t floor,bool sky,Push& out) noexcept {
        Gpu_Climate_Dispatch::RawPush origin{};
        if(!Gpu_Climate_Dispatch::make(cx,cz,origin)||levels.empty()||levels.size()>max_levels||
           sea < -1000000 || sea > 1000000 || water>=Gpu_Base_Region::fallback ||
           floor>=Gpu_Base_Region::fallback) return false;
        Push next{};
        next.count=uint32_t(levels.size());next.flags=Gpu_Base_Region::enabled|(sky?Gpu_Base_Region::sky:0u);
        next.sea=sea;next.water_floor=water|(floor<<16u);
        for(size_t i=0;i<levels.size();++i){
            if(levels[i]<-3125000||levels[i]>3125000) return false;
            for(size_t j=0;j<i;++j) if(levels[i]==levels[j]) return false;
            next.ys[i]=levels[i];
        }
        out=next;return true;
    }
    static bool cave_source(int32_t cx,int32_t cz,int32_t sea,uint32_t count,
                            std::span<const Gpu_Cave_Dispatch::BatchColumn> batch,CavePush& out) noexcept {
        Gpu_Cave_Dispatch::Push origin{};
        if(!count||count>max_levels||batch.empty()||batch.size()>Gpu_Cave_Dispatch::max_columns||
           !Gpu_Cave_Dispatch::make(cx,cz,sea,origin)) return false;
        size_t index=batch.size();
        for(size_t i=0;i<batch.size();++i) if(batch[i].origin_x==origin.origin_x&&batch[i].origin_z==origin.origin_z){
            if(index!=batch.size()||batch[i].sea!=sea) return false;
            index=i;
        }
        if(index==batch.size()) return false;
        out={uint32_t(index),uint32_t(batch.size()),count};return true;
    }
};
static_assert(sizeof(Gpu_Column_Material_Input::Push)==52);
static_assert(offsetof(Gpu_Column_Material_Input::Push,ys)==16);
static_assert(sizeof(Gpu_Column_Material_Input::CavePush)==12);
static_assert(Gpu_Column_Material_Input::header_words==4107);
static_assert(Gpu_Column_Material_Input::words(9)==Gpu_Cave_Dispatch::material_words);
