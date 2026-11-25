#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include "OpenSimplex2S.h"

// Independent std430 bindings. The field order is part of the shader ABI.
struct Gpu_Climate_Tables {
    static constexpr size_t fields=8, period=2048, lattice_count=32;
    enum Field : size_t { Temperature, Humidity, Continental, Erosion, Weirdness, Terrain, Detail, Mountain };
    struct alignas(16) Gradient { double x,y; };
    struct alignas(16) Lattice { double dx,dy; int32_t x,y,pad0,pad1; };
    std::array<int32_t,fields*period> permutation{};
    std::array<Gradient,fields*period> gradient{};
    std::array<Lattice,lattice_count> lattice{};
    // Copy the already seeded fields, never regenerate seeds independently.
    static bool build(const OpenSimplexEnv* env,
                      const std::array<const OpenSimplexGradients*,fields>& field,
                      Gpu_Climate_Tables& out) {
        if (!env || !env->LOOKUP_2D) return false;
        for (auto* f:field) if (!f || !f->perm || !f->permGrad2) return false;
        Gpu_Climate_Tables candidate;
        for(size_t i=0;i<lattice_count;++i) {
            auto* p=env->LOOKUP_2D[i]; if(!p) return false;
            candidate.lattice[i]={p->dx,p->dy,p->xsv,p->ysv,0,0};
        }
        for(size_t f=0;f<fields;++f) for(size_t i=0;i<period;++i) {
            const auto k=f*period+i;
            const auto p=field[f]->perm[i];
            if(p<0 || p>=int(period)) return false;
            candidate.permutation[k]=p;
            candidate.gradient[k]={field[f]->permGrad2[i].dx,field[f]->permGrad2[i].dy};
        }
        out=candidate; return true;
    }
};
static_assert(sizeof(Gpu_Climate_Tables::Gradient)==16);
static_assert(sizeof(Gpu_Climate_Tables::Lattice)==32);
static_assert(offsetof(Gpu_Climate_Tables::Lattice,x)==16);

struct Gpu_Climate_Dispatch {
    // 33x33 includes east/north terrain halo. Interior output: 32x32.
    static constexpr uint32_t side=33, samples=side*side, columns=32*32, local=64;
    static constexpr uint32_t raw_groups=(samples+local-1)/local, terrain_groups=columns/local;
    // Restrict the experimental path to coordinates that also keep the CPU's
    // lattice integer conversions safe. Normal game coordinates fit here.
    static constexpr int64_t limit=100000000;
    struct RawPush { int32_t origin_x,origin_z; };
    struct TerrainPush { int32_t min_y,max_y; };
    static bool make(int32_t chunk_x,int32_t chunk_z,RawPush& out) {
        int64_t x=int64_t(chunk_x)*32,z=int64_t(chunk_z)*32;
        if(x < -limit || z < -limit || x+32 > limit || z+32 > limit) return false;
        out={int32_t(x),int32_t(z)};return true;
    }
    struct alignas(8) Raw { double value[8]; };
    struct alignas(8) Terrain {
        double height, slope, temperature, humidity, continental, erosion, weirdness, mountain;
    };
    // Diagnostic parity only: no ownership certificate. Every sample must
    // be finite and close, and floor(height) must agree exactly.
    static bool matches(std::span<const Terrain> gpu,std::span<const Terrain> cpu,
                        double tolerance=1e-9) {
        if(gpu.size()!=columns || cpu.size()!=columns || !std::isfinite(tolerance) ||
           tolerance<0 || tolerance>1e-7) return false;
        for(size_t i=0;i<columns;++i) {
            const auto& a=gpu[i];const auto& b=cpu[i];
            const std::array<double,8> av{a.height,a.slope,a.temperature,a.humidity,a.continental,a.erosion,a.weirdness,a.mountain};
            const std::array<double,8> bv{b.height,b.slope,b.temperature,b.humidity,b.continental,b.erosion,b.weirdness,b.mountain};
            for(size_t k=0;k<8;++k)
                if(!std::isfinite(av[k]) || !std::isfinite(bv[k]) || std::abs(av[k]-bv[k])>tolerance) return false;
            if(std::floor(a.height)!=std::floor(b.height)) return false;
        }
        return true;
    }
};
static_assert(sizeof(Gpu_Climate_Dispatch::RawPush)==8 && sizeof(Gpu_Climate_Dispatch::TerrainPush)==8);
static_assert(sizeof(Gpu_Climate_Dispatch::Raw)==64 && sizeof(Gpu_Climate_Dispatch::Terrain)==64);
