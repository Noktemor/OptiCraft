#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
using Block_ID=uint16_t;
inline constexpr size_t k_max_block_types=65536;
namespace glm {
struct vec3 {
 float x=0,y=0,z=0;
 float operator[](int i)const {return i==0?x:i==1?y:z;}
};
struct vec4 {float x=0,y=0,z=0,w=0;};
}
enum class Block_Mesh_Type:uint8_t{Solid,Flora,Liquid,Shaped};
enum class Block_Mesh_Style:uint8_t{Block,XStyle};
enum class Block_Orientation:uint8_t{None,Axis,Facing};
enum class Block_Shape:uint8_t{None,Slab,Stairs,Plate,Count=33};
struct Block_Properties {
 bool is_item=false,is_opaque=false,is_transparent=false,uses_meta=false,has_active_state=false;
 Block_Mesh_Type mesh_type=Block_Mesh_Type::Solid;
 Block_Mesh_Style mesh_style=Block_Mesh_Style::Block;
 Block_Orientation orientation=Block_Orientation::None;
 Block_Shape shape=Block_Shape::None;
 glm::vec4 base{0.1f,0.2f,0.3f,0.4f}, alternate{0.5f,0.6f,0.7f,0.8f};
 const glm::vec4& resolve_face_uv(int, uint8_t meta,bool& rot)const {
  rot=meta==1;return meta==1?alternate:base;
 }
};
class Block_Registry {
public:
 explicit Block_Registry(size_t n):props_(n),known_(n){}
 size_t id_capacity()const{return props_.size();}
 bool is_known(Block_ID id)const{return known_[id];}
 const Block_Properties& get_properties(Block_ID id)const{return props_[id];}
 Block_Properties& edit(size_t id){known_[id]=true;return props_[id];}
private:
 std::vector<Block_Properties> props_;
 std::vector<bool> known_;
};
