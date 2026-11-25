#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <span>
#include <algorithm>
#include <cmath>


// Diagnostic hybrid cave pipeline. CPU emits original path/chamber primitives;
// GPU rasterizes and classifies them. All buffers are std430.
struct Gpu_Cave_Dispatch {
    static constexpr int side=32, halo=34, min_y=-150, max_y=130, height=281;
    static constexpr uint32_t mask_words=height*side, kind_words=mask_words*4;
    static constexpr uint32_t max_stamps=65535, resolve_groups=(kind_words+63)/64;
    enum : uint32_t { Network=0, Shaft=1, Sealed=2, Flooded=3 };
    struct alignas(8) Stamp { double x,y,z,rxz,ry; uint32_t type,pad; };
    static constexpr uint32_t pack_groups=32;
    struct PackPush { int32_t chunk_y; uint32_t level; };
    static bool make_pack(int chunk_y,uint32_t level,uint32_t levels,PackPush& out) {
        if(levels==0 || levels>9 || level>=levels || chunk_y < -3125000 || chunk_y > 3125000) return false;
        out={chunk_y,level};return true;
    }
    struct Push { int32_t origin_x,origin_z,sea; uint32_t count; };
    struct alignas(16) Gradient { double x,y; };
    struct alignas(16) Lattice { double dx,dy; int32_t x,y,pad0,pad1; };
    struct Tables {
        std::array<int32_t,2048> permutation;
        std::array<Gradient,2048> gradient;
        std::array<Lattice,32> lattice;
    };
    struct Packet {
        Push push{};
        std::array<int32_t,halo*halo> heights{};
        std::vector<Stamp> stamps;
        Tables tables{};
        bool valid_for(int cx,int cz) const {
            Push expected{};
            if(!make(cx,cz,push.sea,expected)||expected.origin_x!=push.origin_x||expected.origin_z!=push.origin_z||push.count!=stamps.size()||push.count>max_stamps)return false;
            for(auto v:tables.permutation)if(v<0||v>=2048)return false;
            for(const auto& g:tables.gradient)if(!std::isfinite(g.x)||!std::isfinite(g.y))return false;
            for(const auto& l:tables.lattice)if(!std::isfinite(l.dx)||!std::isfinite(l.dy)||l.x < -4096||l.x>4096||l.y < -4096||l.y>4096)return false;
            for(const auto& s:stamps)if(!std::isfinite(s.x)||!std::isfinite(s.y)||!std::isfinite(s.z)||!std::isfinite(s.rxz)||!std::isfinite(s.ry)||std::abs(s.x)>100000000||std::abs(s.z)>100000000||std::abs(s.y)>1000000||s.rxz<=0||s.ry<=0||s.rxz>1000000||s.ry>1000000||s.type>Flooded)return false;
            return true;
        }
    };
    struct RegionStamps { std::array<std::vector<Stamp>,64> bins; };
    static constexpr uint32_t max_columns=8;
    struct Coord { int32_t x,z; };
    struct BatchColumn { int32_t origin_x,origin_z,sea; uint32_t count,first_stamp; };
    struct BatchPush { uint32_t columns; };
    static constexpr uint32_t material_levels=9, material_words=4107+material_levels*2048;
    struct BatchLevel { int32_t chunk_y=0; uint32_t enabled=0; };
    using BatchLevels=std::array<BatchLevel,max_columns*material_levels>;
    static bool set_batch_level(BatchLevels& levels,uint32_t columns,uint32_t column,uint32_t level,int chunk_y) {
        PackPush p{};
        if(columns==0 || columns>max_columns || column>=columns || !make_pack(chunk_y,level,material_levels,p)) return false;
        levels[column*material_levels+level]={chunk_y,1};return true;
    }
    struct Batch {
        Tables tables{};
        std::vector<BatchColumn> columns;
        std::vector<int32_t> heights;
        std::vector<Stamp> stamps;
        uint32_t carve_groups=0;
        bool valid() const {
            if(columns.empty() || columns.size()>max_columns || heights.size()!=columns.size()*halo*halo) return false;
            size_t offset=0; uint32_t largest=0;
            for(size_t i=0;i<columns.size();++i) {
                const auto& c=columns[i]; Push p{};
                if(c.origin_x%side || c.origin_z%side || !make(c.origin_x/side,c.origin_z/side,c.sea,p) ||
                   c.count>max_stamps || c.first_stamp!=offset || offset+c.count>stamps.size()) return false;
                for(size_t j=0;j<i;++j) if(columns[j].origin_x==c.origin_x && columns[j].origin_z==c.origin_z) return false;
                offset+=c.count; largest=std::max(largest,c.count);
            }
            return offset==stamps.size() && largest==carve_groups;
        }
    };
    static bool make(int cx,int cz,int sea,Push& out) {
        constexpr int64_t limit=100000000-1024;
        const int64_t x=int64_t(cx)*side,z=int64_t(cz)*side;
        if(x < -limit || z < -limit || x+side > limit || z+side > limit || sea < -1000000 || sea > 1000000) return false;
        out={int32_t(x),int32_t(z),sea,0};return true;
    }
    // Four-bit Cave_Kind values in y,z,x order. Reject unused bits/kinds too.
    static bool matches(std::span<const uint32_t> a,std::span<const uint32_t> b) {
        if(a.size()!=kind_words || b.size()!=kind_words) return false;
        for(size_t i=0;i<a.size();++i) {
            if(a[i]!=b[i]) return false;
            for(int k=0;k<8;++k) if(((a[i]>>(4*k))&15u)>4u) return false;
        }
        return true;
    }
};
static_assert(sizeof(Gpu_Cave_Dispatch::Stamp)==48 && offsetof(Gpu_Cave_Dispatch::Stamp,type)==40);
static_assert(sizeof(Gpu_Cave_Dispatch::Push)==16);
static_assert(sizeof(Gpu_Cave_Dispatch::PackPush)==8 && sizeof(Gpu_Cave_Dispatch::Gradient)==16 && sizeof(Gpu_Cave_Dispatch::Lattice)==32);

static_assert(sizeof(Gpu_Cave_Dispatch::BatchColumn)==20 && offsetof(Gpu_Cave_Dispatch::BatchColumn,first_stamp)==16);
static_assert(sizeof(Gpu_Cave_Dispatch::BatchPush)==4);

static_assert(sizeof(Gpu_Cave_Dispatch::BatchLevel)==8);
