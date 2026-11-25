#pragma once
#include "world/generation/Gpu_Climate_Data.h"
#include <cmath>
#include <cstdint>
#include <span>
#include <cstddef>

// Diagnostic GPU column metadata. This is not Gpu_Material_Column: the last
// word contains surface/mantle IDs, not the material shader's enable flags.
struct Gpu_Layer_Dispatch {
    static constexpr uint32_t columns=Gpu_Climate_Dispatch::columns;
    static constexpr uint32_t groups=Gpu_Climate_Dispatch::terrain_groups;
    struct Push { int32_t origin_x,origin_z; uint32_t seed,single_biome,selected_biome; };
    struct alignas(8) Layer { double height; uint32_t biome,packed_layers; };
    static constexpr uint32_t pack(uint16_t surface,uint16_t mantle) noexcept {
        return uint32_t(surface)|(uint32_t(mantle)<<16u);
    }
    static bool make(int32_t chunk_x,int32_t chunk_z,uint32_t seed,bool single_biome,
                     uint32_t selected_biome,uint32_t biome_count,Push& out) {
        Gpu_Climate_Dispatch::RawPush p;
        if(selected_biome>=biome_count || !Gpu_Climate_Dispatch::make(chunk_x,chunk_z,p)) return false;
        out={p.origin_x,p.origin_z,seed,uint32_t(single_biome),selected_biome};return true;
    }
    // Both the top voxel and the lowest mantle voxel must match. An exact
    // integral height and an infinitesimally higher height have equal floor,
    // but different ceil(height-4), so floor alone is not sufficient.
    static bool matches(std::span<const Layer> gpu,std::span<const Layer> cpu,
                        uint32_t biome_count,uint32_t block_count,double tolerance=1e-9) {
        if(gpu.size()!=columns || cpu.size()!=columns || !biome_count || !block_count ||
           !std::isfinite(tolerance) || tolerance<0 || tolerance>1e-7) return false;
        auto valid=[&](const Layer& l) {
            return std::isfinite(l.height) && l.biome<biome_count &&
                   (l.packed_layers&0xffffu)<block_count && (l.packed_layers>>16u)<block_count &&
                   l.packed_layers!=0xffffffffu;
        };
        for(size_t i=0;i<columns;++i) {
            const auto& a=gpu[i];const auto& b=cpu[i];
            if(!valid(a)||!valid(b)||a.biome!=b.biome||a.packed_layers!=b.packed_layers||
               std::abs(a.height-b.height)>tolerance||std::floor(a.height)!=std::floor(b.height)||
               std::ceil(a.height-4.0)!=std::ceil(b.height-4.0)) return false;
        }
        return true;
    }
};
static_assert(sizeof(Gpu_Layer_Dispatch::Push)==20);
static_assert(sizeof(Gpu_Layer_Dispatch::Layer)==16);
static_assert(offsetof(Gpu_Layer_Dispatch::Layer,biome)==8);
static_assert(offsetof(Gpu_Layer_Dispatch::Layer,packed_layers)==12);
