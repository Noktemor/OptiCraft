#pragma once
#include "world/chunks/Chunk.h"
struct Renderable_Chunk {struct Position {int x,y,z;} position;};
class Chunk_Manager {
public:
    std::array<Chunk,3> chunks;
    std::unordered_map<OptiCraft::Vulkan::ChunkKey,Chunk,OptiCraft::Vulkan::ChunkKeyHash> extra;
    uint64_t epoch=1;
    uint64_t vulkan_resident_epoch() const{return epoch;}
    const Chunk* get_chunk(int x,int y,int z) const {
        if(x>=0 && x<3 && y==0 && z==0)return &chunks[size_t(x)];
        const auto it=extra.find({0,x,y,z});
        return it==extra.end()?nullptr:&it->second;
    }
};
