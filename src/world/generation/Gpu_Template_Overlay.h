#pragma once
#include "world/generation/Structure_Pack.h"
#include "world/generation/Tree_Pack.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <tuple>
#include <vector>

// Ordered, immutable local-space template data. One invocation owns four
// voxels (two packed block words and one metadata word). The predicate reads
// the original procedural base, never a prior decoration's output.
namespace Gpu_Template_Overlay {
static_assert(static_cast<Block_ID>(Block_Types::Air)==0 && static_cast<Block_ID>(Block_Types::Water)==7);
static_assert(Block_Meta::direction_mask==3);
constexpr uint32_t max_records=1u<<20, max_instances=128, max_templates=1024;
constexpr uint32_t block_words=16384, meta_words=8192;
enum class Predicate : uint32_t { Always=0, Air=1, Water=2 };
struct Record { int32_t x,y,z; uint32_t value; }; // low16 ID, next8 meta, bit24 rotate
struct Instance {
    int32_t x,y,z; uint32_t rotation;
    uint32_t begin,end,predicate,reserved;
    int32_t min_x,min_y,min_z,pad0;
    int32_t max_x,max_y,max_z,pad1;
};
static_assert(sizeof(Record)==16 && sizeof(Instance)==64);
inline auto key(const Record& r) noexcept { return std::tuple{r.x,r.y,r.z}; }
struct Range { uint32_t begin,end; std::array<int32_t,3> lo,hi; };
class Packet {
    std::vector<Record> records_;
    std::vector<Instance> instances_;
    std::vector<Range> ranges_;
public:
    std::span<const Record> records() const noexcept {return records_;}
    std::span<const Instance> instances() const noexcept {return instances_;}
    std::span<const Range> ranges() const noexcept {return ranges_;}
    // Stable sort preserves repeated writes at the same voxel (including Air,
    // zero direction meta, and same-ID writes). No last-writer collapsing.
    bool add_template(std::span<const Record> source,uint32_t& handle) {
        if(source.empty() || source.size()>max_records-records_.size() || ranges_.size()>=max_templates)return false;
        Range r{uint32_t(records_.size()),0,{INT32_MAX,INT32_MAX,INT32_MAX},{INT32_MIN,INT32_MIN,INT32_MIN}};
        for(const auto& v:source) {
            if(v.x < -65535 || v.x > 65535 || v.y < -65535 || v.y > 65535 || v.z < -65535 || v.z > 65535 ||
               (v.value&0xfe000000u) || (v.value&65535u)==65535u)return false;
            std::array<int32_t,3> p{v.x,v.y,v.z};
            for(unsigned k=0;k<3;++k){r.lo[k]=std::min(r.lo[k],p[k]);r.hi[k]=std::max(r.hi[k],p[k]);}
        }
        std::vector<Record> sorted(source.begin(),source.end());
        std::stable_sort(sorted.begin(),sorted.end(),[](auto& a,auto& b){return key(a)<key(b);});
        records_.insert(records_.end(),sorted.begin(),sorted.end());r.end=uint32_t(records_.size());
        handle=uint32_t(ranges_.size());ranges_.push_back(r);return true;
    }
    bool add_tree(std::span<const Tree_Pack::Block> tree,uint32_t& handle){
        if(tree.empty()||tree.size()>max_records-records_.size())return false;
        std::vector<Record> v;v.reserve(tree.size());
        for(auto b:tree)v.push_back({b.x,b.y,b.z,static_cast<Block_ID>(b.type)});
        return add_template(v,handle);
    }
    bool add_structure(const Structure_Template& t,uint32_t& handle){
        if(t.voxels.empty()||t.voxels.size()>max_records-records_.size()||t.entrance_x<0||t.entrance_x>65535||t.entrance_z<0||t.entrance_z>65535)return false;
        std::vector<Record> v;v.reserve(t.voxels.size());
        for(auto b:t.voxels){if(b.flags>1)return false;v.push_back({int(b.x)-t.entrance_x,b.y,int(b.z)-t.entrance_z,
            uint32_t(static_cast<Block_ID>(b.type))|(uint32_t(b.meta)<<16)|(uint32_t(b.flags)<<24)});}
        return add_template(v,handle);
    }
    // Coordinates are chunk-relative, not world-space: no 64-bit shader math.
    bool add_instance(uint32_t handle,int64_t x,int64_t y,int64_t z,uint32_t rotation,Predicate predicate){
        constexpr int64_t limit=1<<20;
        if(handle>=ranges_.size()||instances_.size()>=max_instances||rotation>3||uint32_t(predicate)>2||
           x < -limit||x>limit||y < -limit||y>limit||z < -limit||z>limit)return false;
        const auto& r=ranges_[handle];
        instances_.push_back({int32_t(x),int32_t(y),int32_t(z),rotation,r.begin,r.end,uint32_t(predicate),0,
            r.lo[0],r.lo[1],r.lo[2],0,r.hi[0],r.hi[1],r.hi[2],0});return true;
    }
    bool add_world_instance(uint32_t handle,int x,int y,int z,int cx,int cy,int cz,uint32_t rotation,Predicate predicate){
        return add_instance(handle,int64_t(x)-int64_t(cx)*32,int64_t(y)-int64_t(cy)*32,int64_t(z)-int64_t(cz)*32,rotation,predicate);
    }
};
// Local valid Y interval [first,last), including the one-voxel top world level.
inline bool y_interval(int chunk_y,uint32_t& first,uint32_t& last) noexcept {
    int64_t base=int64_t(chunk_y)*32;
    int64_t lo=std::max<int64_t>(0,int64_t(Config::world_min_y)-base);
    int64_t hi=std::min<int64_t>(32,int64_t(Config::world_max_y)+1-base);
    if(lo>=hi)return false;
    first=uint32_t(lo);last=uint32_t(hi);return true;
}
inline bool valid_base(std::span<const uint32_t> base) noexcept {
    return base.size()==block_words&&std::none_of(base.begin(),base.end(),[](uint32_t w){return uint16_t(w)==65535||uint16_t(w>>16)==65535;});
}
// One bounded dispatch; the GPU-resident base must be separately certified
// (valid_base is the host-side check for uploaded bases). Output buffers must
// be preinitialized and must not alias the base or packet buffers.
struct Push { uint32_t instanceCount, firstY, lastY; };
static_assert(sizeof(Push)==12);
constexpr uint32_t dispatch_groups=128;
inline bool prepare_dispatch(const Packet& packet,int chunk_y,Push& output) noexcept {
    uint32_t lo,hi;
    if(!y_interval(chunk_y,lo,hi))return false;
    output={uint32_t(packet.instances().size()),lo,hi};return true;
}
} // namespace Gpu_Template_Overlay
