#pragma once
#include <set>
#include <thread>
#include <tuple>
#include <cassert>
#include "world/blocks/Block_Types.h"
class Chunk_Manager {
public:
    std::thread::id owner=std::this_thread::get_id();
    std::set<std::tuple<int,int,int>> walls;
    unsigned get_block_meta_world(int, int, int) const {
        assert(std::this_thread::get_id()==owner && "worker illegally read block metadata");
        return 0u;
    }
    Block_Types get_block_world(int x,int y,int z) const {
        assert(std::this_thread::get_id()==owner && "worker illegally read Chunk_Manager");
        if(y==-1 || walls.count({x,y,z}))return Block_Types::Stone;
        return Block_Types::Air;
    }
};
