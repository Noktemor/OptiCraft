#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>
#include "rendering/vulkan/GpuWorldCore.h"
// Host-only feed fixture. Not a replacement for the game's real chunk classes.
struct MockBlockGrid {
    std::vector<uint16_t> ids{0};
    std::vector<uint16_t> dense;
    std::vector<uint32_t> meta;
    mutable unsigned unpackCalls=0, metaCalls=0;
    const auto& palette() const {return ids;}
    const auto& meta_entries() const {return meta;}
    bool is_uniform() const {return dense.empty();}
    uint16_t uniform_block() const {return ids[0];}
    void unpack(uint16_t* out) const {
        ++unpackCalls;
        if(is_uniform())std::fill_n(out,OptiCraft::Vulkan::kChunkVoxels,ids[0]);
        else std::copy(dense.begin(),dense.end(),out);
    }
    void unpack_meta(uint8_t* out) const {
        ++metaCalls;
        std::fill_n(out,OptiCraft::Vulkan::kChunkVoxels,0);
        for(size_t i=0;i<meta.size();i+=2)if(meta[i+1])out[meta[i]&0xfffffu]=uint8_t(meta[i+1]);
    }
};
struct MockLightGrid {
    uint8_t uniform=0;
    std::vector<uint8_t> dense;
    mutable unsigned unpackCalls=0;
    bool is_uniform() const {return dense.empty();}
    uint8_t get(int x,int y,int z) const {return is_uniform()?uniform:dense[y*1024+z*32+x];}
    void unpack(uint8_t* out) const {
        ++unpackCalls;
        if(is_uniform())std::fill_n(out,OptiCraft::Vulkan::kChunkVoxels,uniform);
        else std::copy(dense.begin(),dense.end(),out);
    }
};
struct Chunk {
    MockBlockGrid blocks;
    MockLightGrid lights;
    uint64_t instance=1, version=1, dirty=1;
    static uint64_t dirty_event_count(){return 1;}
    uint64_t get_instance_id() const{return instance;}
    uint64_t get_blocks_version() const{return version;}
    uint64_t get_dirty_epoch() const{return dirty;}
    bool has_blocks() const{return std::any_of(blocks.ids.begin(),blocks.ids.end(),[](uint16_t id){return id!=0;});}
    const auto& get_blocks() const{return blocks;}
    const auto& get_light_grid() const{return lights;}
};
