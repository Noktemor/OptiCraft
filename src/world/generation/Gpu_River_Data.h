#pragma once
#include <cstdint>
#include <limits>
#include <vector>
#include <span>
#include <cmath>

// Hybrid hydrology ABI. The deterministic priority-flood and basin graph remain CPU-owned.
struct Gpu_River_Dispatch {
    static constexpr int region=256, margin=256, width=768, coarse_width=193;
    static constexpr uint32_t cells=width*width, coarse_cells=coarse_width*coarse_width;
    static constexpr uint32_t columns=region*region, local_size=64;
    static constexpr uint32_t window_groups=cells/local_size, region_groups=columns/local_size;
    static constexpr int32_t no_fall=std::numeric_limits<int32_t>::min();
    struct Push { int32_t sea; };
    struct SmoothPush { uint32_t axis; }; // 0 = X (int heights), 1 = Z (double intermediate)
    struct BridgePush { uint32_t offset_x,offset_z; };
    struct ColumnPush {int32_t sea;uint32_t offset_x,offset_z;};
    static constexpr uint32_t column_resolved_width=40, column_resolved_cells=1600;
    static constexpr uint32_t column_resolve_groups=25, column_merge_groups=16;
    // Mathematical floor, including negative chunks on region boundaries.
    static constexpr int32_t region_of_chunk(int32_t c) noexcept {return c/8-(c%8<0?1:0);}
    struct Cell { int32_t area, lake, lake_level; };
    struct Profile { int32_t river,top,water,bed,fall,lake; bool operator==(const Profile&) const=default; };
    struct Packet {
        int32_t region_x=0, region_z=0, sea=0;
        std::vector<int32_t> coarse, height;
        std::vector<double> smooth;
        std::vector<Cell> graph;
        std::vector<Profile> reference;
        bool valid() const {
            if (!valid_region(region_x,region_z) || sea < -32768 || sea > 32767 ||
                coarse.size()!=coarse_cells || height.size()!=cells || smooth.size()!=cells ||
                graph.size()!=cells || reference.size()!=columns) return false;
            for(auto h:coarse) if(h < -32768 || h > 32767) return false;
            for(uint32_t i=0;i<cells;++i) {
                const auto& c=graph[i];
                if(height[i]<-32768 || height[i]>32767 || !std::isfinite(smooth[i]) ||
                   c.area<1 || c.area>int32_t(cells) || (c.lake!=0 && c.lake!=1) ||
                   c.lake_level < -32768 || c.lake_level > 32767 || (!c.lake && c.lake_level!=0)) return false;
            }
            return true;
        }
    };
    // Runtime packet: only the interior plus the waterfall (4) and river
    // widening (1) halos. No diagnostic full-window heights/smooth/reference.
    static constexpr uint32_t compact_halo=5, compact_width=region+2*compact_halo;
    static constexpr uint32_t resolved_width=region+8, compact_cells=compact_width*compact_width;
    static constexpr uint32_t resolved_cells=resolved_width*resolved_width;
    static constexpr uint32_t compact_coarse_width=69, compact_coarse_cells=compact_coarse_width*compact_coarse_width;
    static constexpr uint32_t compact_coarse_start=(margin-compact_halo)/4;
    static constexpr uint32_t compact_bytes=4*(compact_cells+compact_coarse_cells);
    static constexpr uint32_t lake_bit=0x10000u;
    static constexpr uint32_t compact_groups=(resolved_cells+local_size-1)/local_size;
    static bool encode_cell(int32_t area,bool lake,int32_t level,uint32_t& word) noexcept {
        if(area<1||area>int32_t(cells)||level < -32768||level>32767||(!lake&&level!=0))return false;
        word=lake?(lake_bit|uint32_t(uint16_t(level))):(area>=1600?2u:area>=400?1u:0u);
        return true;
    }
    struct CompactPacket {
        int32_t region_x=0,region_z=0,sea=0;
        std::vector<int32_t> coarse;
        std::vector<uint32_t> graph;
        bool valid() const noexcept {
            if(!valid_region(region_x,region_z)||sea < -32768||sea>32767||
               coarse.size()!=compact_coarse_cells||graph.size()!=compact_cells)return false;
            for(int32_t h:coarse)if(h < -32768||h>32767)return false;
            for(uint32_t w:graph)if(w>2u&&(w<lake_bit||w>0x1ffffu))return false;
            return true;
        }
    };
    static constexpr bool valid_region(int32_t rx,int32_t rz) {
        // Match the bounded coordinate domain used by the existing GPU worldgen.
        auto valid=[](int32_t r){return int64_t(r)*region-margin>=-100000000 &&
            int64_t(r)*region+region+margin<=100000000;};
        return valid(rx)&&valid(rz);
    }
    static bool bridge(int32_t rx,int32_t rz,int32_t cx,int32_t cz,BridgePush& out) {
        if(!valid_region(rx,rz)) return false;
        const int64_t x=int64_t(cx)*32-int64_t(rx)*region,z=int64_t(cz)*32-int64_t(rz)*region;
        if(x<0||z<0||x>region-32||z>region-32) return false;
        out={uint32_t(x),uint32_t(z)};return true;
    }
    static bool matches(std::span<const Profile> gpu,std::span<const Profile> cpu) {
        if(gpu.size()!=columns || cpu.size()!=columns) return false;
        for(size_t i=0;i<columns;++i) if(!(gpu[i]==cpu[i])) return false;
        return true;
    }
};
static_assert(sizeof(Gpu_River_Dispatch::Cell)==12);
static_assert(sizeof(Gpu_River_Dispatch::Profile)==24);
static_assert(sizeof(Gpu_River_Dispatch::BridgePush)==8);
static_assert(sizeof(Gpu_River_Dispatch::ColumnPush)==12);
