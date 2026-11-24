#pragma once
#include <glm/glm.hpp>
#include <map>
#include <tuple>
#include <string>
enum class Block_Types { Air=0, Stone=5, Oak_Log=56, Fire=529 };
struct Block_Properties { std::string name; bool is_solid=false; };
inline Block_Properties get_block_props(Block_Types t) {
 switch (t) {
 case Block_Types::Stone: return {"Stone",true};
 case Block_Types::Oak_Log: return {"Oak Log",true};
 case Block_Types::Fire: return {"Fire",false};
 default: return {"Air",false};
 }
}
class Chunk_Manager {
 std::map<std::tuple<int,int,int>, Block_Types> blocks;
 public:
 Block_Types get_block_world(int x,int y,int z) const { auto it=blocks.find({x,y,z});return it!=blocks.end()?it->second:Block_Types::Air; }
 bool set_block_world(int x,int y,int z,Block_Types type){blocks[{x,y,z}]=type;return true;}
 void* get_chunk(int,int,int){return this;}
};
