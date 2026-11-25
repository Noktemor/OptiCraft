#pragma once
#include "world/chunks/Chunk.h"
struct Renderable_Chunk {struct Position {int x,y,z;} position;};
class Chunk_Manager {
public:
    std::array<Chunk,3> chunks;
    uint64_t vulkan_resident_epoch() const{return 1;}
    const Chunk* get_chunk(int x,int y,int z) const {
        return x>=0 && x<3 && y==0 && z==0?&chunks[size_t(x)]:nullptr;
    }
};
