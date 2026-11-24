#include "world/logic/Logic_Manager.h"
#include "world/blocks/Block_Shapes.h"
#include <map>
#include <tuple>
#include <cassert>
#include <cstdio>
#include <cmath>
using Key=std::tuple<int,int,int>;
struct W {
 std::map<Key,std::pair<Block_Types,uint8_t>> m;
 int high=1000;
 void put(int x,int y,int z,Block_Types t,uint8_t meta=0){m[{x,y,z}]={t,meta};}
 Block_Types t(int x,int y,int z)const{auto i=m.find({x,y,z});return i==m.end()?Block_Types::Air:i->second.first;}
 uint8_t me(int x,int y,int z)const{auto i=m.find({x,y,z});return i==m.end()?0:i->second.second;}
 Logic_Manager::World_Access access(){return {[this](int x,int y,int z){return t(x,y,z);},
 [this](int x,int y,int z){return me(x,y,z);},
 [this](int x,int y,int z,Block_Types b,uint8_t d){put(x,y,z,b,d);},
 [this](int x,int y,int z){return x<high;}};}
};
int main(){
  // mounting: new wall wire supported on +X, old meta=0 still floor
  W w; w.put(2,0,0,Block_Types::Stone);w.put(1,-1,0,Block_Types::Stone);
  uint8_t meta=255;auto a=w.access();
  assert(Logic_Manager::placement(Block_Types::Copper_Wire,a,{1,0,0},{2,0,0},0,meta)&&meta==1);
  assert(Logic_Manager::placement(Block_Types::Copper_Wire,a,{1,0,0},{1,-1,0},0,meta)&&meta==0);
  assert(Logic_Manager::support_of({1,0,0},Block_Types::Copper_Wire,1)==glm::ivec3(2,0,0));
  Shape_Box boxes[16];assert(shape_boxes(Block_Shape::Wire,static_cast<uint8_t>(0|(5<<3)),boxes)==1);
  assert(boxes[0].lo.z==0.0f && boxes[0].hi.z==1.0f);
  assert(shape_boxes(Block_Shape::Wire,static_cast<uint8_t>(1|(3<<3)),boxes)==3);
  for(int i=0;i<3;++i)for(float v:{boxes[i].lo.x,boxes[i].lo.y,boxes[i].lo.z,boxes[i].hi.x,boxes[i].hi.y,boxes[i].hi.z})assert(v>=0&&v<=1);
  // All six button mounting faces use a 6x4 face and a 2px shaft.
  // When pressed the protrusion shrinks to exactly 1px (meta bit 3).
  for (uint8_t mount = 0; mount < 6; ++mount) {
      Shape_Box up[16], down[16];
      assert(shape_boxes(Block_Shape::Button, mount, up) == 1);
      assert(shape_boxes(Block_Shape::Button, static_cast<uint8_t>(mount | 8u), down) == 1);
      auto extent = [](const Shape_Box& b, int ax) {
          return (ax == 0 ? b.hi.x - b.lo.x : ax == 1 ? b.hi.y - b.lo.y : b.hi.z - b.lo.z);
      };
      const int axis = mount / 2;
      assert(std::abs(extent(up[0], axis) - 2.0f / 16.0f) < 1e-6f);
      assert(std::abs(extent(down[0], axis) - 1.0f / 16.0f) < 1e-6f);
      const int a = (axis + 1) % 3, b = (axis + 2) % 3;
      const float e1 = extent(up[0], a), e2 = extent(up[0], b);
      assert((std::abs(e1-6.0f/16.0f)<1e-6f && std::abs(e2-4.0f/16.0f)<1e-6f) ||
             (std::abs(e2-6.0f/16.0f)<1e-6f && std::abs(e1-4.0f/16.0f)<1e-6f));
      for(float v : {up[0].lo.x, up[0].lo.y, up[0].lo.z,
                     up[0].hi.x, up[0].hi.y, up[0].hi.z}) assert(v >= 0 && v <= 1);
  }
  // lever / wire / two-cell door
  W d;d.put(0,0,0,Block_Types::Lever_On);d.put(1,0,0,Block_Types::Copper_Wire,0);
  d.put(2,0,0,Block_Types::Copper_Wire,0);d.put(3,0,0,Block_Types::Door);
  d.put(3,1,0,Block_Types::Door,8);Logic_Manager logic;
  logic.track({0,0,0});logic.track({1,0,0});logic.track({2,0,0});
  logic.step(d.access()); assert((d.me(3,0,0)&4)!=0 && (d.me(3,1,0)&4)!=0);
  d.put(0,0,0,Block_Types::Lever);logic.step(d.access());assert((d.me(3,0,0)&4)==0);
  // observer sees watched block's type/meta and pulses for one step
  W o;o.put(10,0,0,Block_Types::Observer,1);Logic_Manager obs;obs.track({10,0,0});
  obs.step(o.access());assert(o.me(10,0,0)==1);
  o.put(11,0,0,Block_Types::Stone);obs.step(o.access());assert((o.me(10,0,0)&8)!=0);
  obs.step(o.access());assert((o.me(10,0,0)&8)==0);
  // piston pushes block; blocked across unloaded chunk, sticky piston retracts
  W p;p.put(20,0,0,Block_Types::Piston,1);p.put(21,0,0,Block_Types::Stone);p.put(20,1,0,Block_Types::Lever_On);
  Logic_Manager mech;mech.track({20,0,0});mech.track({20,1,0});mech.step(p.access());
  assert((p.me(20,0,0)&8)!=0&&p.t(21,0,0)==Block_Types::Piston_Head&&p.t(22,0,0)==Block_Types::Stone);
  p.put(20,1,0,Block_Types::Lever);mech.step(p.access());
  assert((p.me(20,0,0)&8)==0 && p.t(21,0,0)==Block_Types::Air && p.t(22,0,0)==Block_Types::Stone);
  W q;q.put(30,0,0,Block_Types::Sticky_Piston,1);q.put(31,0,0,Block_Types::Stone);q.put(30,1,0,Block_Types::Lever_On);
  Logic_Manager sm;sm.track({30,0,0});sm.track({30,1,0});sm.step(q.access());
  q.put(30,1,0,Block_Types::Lever);sm.step(q.access());
  assert(q.t(31,0,0)==Block_Types::Stone && q.t(32,0,0)==Block_Types::Air);
  W blocked; blocked.high=32; blocked.put(30,0,0,Block_Types::Piston,1);
  blocked.put(31,0,0,Block_Types::Stone);blocked.put(30,1,0,Block_Types::Lever_On);
  Logic_Manager bm;bm.track({30,0,0});bm.track({30,1,0});bm.step(blocked.access());
  assert((blocked.me(30,0,0)&8)==0 && blocked.t(31,0,0)==Block_Types::Stone);
  // Floor-to-wall riser: both signal and shape use the same attachment direction.
  W wall;wall.put(-1,0,0,Block_Types::Lever_On);
  wall.put(0,0,0,Block_Types::Copper_Wire,0);
  wall.put(1,0,0,Block_Types::Copper_Wire,1);
  wall.put(1,1,0,Block_Types::Copper_Wire,1);
  wall.put(1,2,0,Block_Types::Copper_Lamp);
  auto wall_type=[&](glm::ivec3 n){return wall.t(n.x,n.y,n.z);};
  auto wall_meta=[&](glm::ivec3 n){return wall.me(n.x,n.y,n.z);};
  const uint8_t floor_shape=wire_shape_meta(0,{0,0,0},wall_type,wall_meta);
  const uint8_t wall_shape=wire_shape_meta(1,{1,0,0},wall_type,wall_meta);
  assert((floor_shape&(2u<<3))!=0u && (wall_shape&128u)!=0u);
  assert(shape_boxes(Block_Shape::Wire,wall_shape,boxes)>=3);
  Logic_Manager wall_logic;
  for(auto q:{glm::ivec3(-1,0,0),glm::ivec3(0,0,0),glm::ivec3(1,0,0),glm::ivec3(1,1,0),glm::ivec3(1,2,0)})wall_logic.track(q);
  wall_logic.step(wall.access());assert(wall.t(1,2,0)==Block_Types::Copper_Lamp_On);
  // Wall-mounted parallel wires with different supports must not short together.
  W isolated;isolated.put(6,-1,0,Block_Types::Lever_On);
  isolated.put(6,0,0,Block_Types::Copper_Wire,1);
  isolated.put(6,1,0,Block_Types::Copper_Wire,2);
  isolated.put(6,2,0,Block_Types::Copper_Lamp);
  Logic_Manager il;for(int y=-1;y<=2;++y)il.track({6,y,0});il.step(isolated.access());
  assert(isolated.t(6,2,0)==Block_Types::Copper_Lamp);
  // Full-cube redstone devices are not support-dependent when blocks beneath break.
  W supports;supports.put(1,1,1,Block_Types::Piston,1);
  supports.put(2,1,1,Block_Types::Observer,1);
  supports.put(3,1,1,Block_Types::Copper_Lamp);
  supports.put(4,1,1,Block_Types::Copper_Wire,0);
  supports.put(5,1,1,Block_Types::Repeater,0);
  auto broken=Logic_Manager::nodes_without_support(supports.access(),{3,0,1});
  for(const auto& pos:broken)assert(pos!=glm::ivec3(3,1,1));
  assert(Logic_Manager::has_support_for(supports.access(),{1,1,1},Block_Types::Piston,1));
  assert(Logic_Manager::has_support_for(supports.access(),{2,1,1},Block_Types::Observer,1));
  assert(Logic_Manager::has_support_for(supports.access(),{3,1,1},Block_Types::Copper_Lamp,0));
  assert(!Logic_Manager::has_support_for(supports.access(),{4,1,1},Block_Types::Copper_Wire,0));
  assert(!Logic_Manager::has_support_for(supports.access(),{5,1,1},Block_Types::Repeater,0));
  // A chain of 15+ dust cells must decay to zero.
  W longline;longline.put(-1,0,0,Block_Types::Lever_On);
  Logic_Manager ll;ll.track({-1,0,0});
  for(int i=0;i<16;++i){longline.put(i,0,0,Block_Types::Copper_Wire);ll.track({i,0,0});}
  longline.put(16,0,0,Block_Types::Copper_Lamp);ll.track({16,0,0});
  ll.step(longline.access()); assert(longline.t(16,0,0)==Block_Types::Copper_Lamp);
  // Limit is 12 movable blocks (Minecraft Java piston rule).
  for(int count:{12,13}){
    W chain;chain.put(100,0,0,Block_Types::Piston,1);
    chain.put(100,1,0,Block_Types::Lever_On);
    for(int x=101;x<101+count;++x)chain.put(x,0,0,Block_Types::Stone);
    Logic_Manager pm;pm.track({100,0,0});pm.track({100,1,0});pm.step(chain.access());
    if(count==12) assert((chain.me(100,0,0)&8)!=0 && chain.t(113,0,0)==Block_Types::Stone);
    else assert((chain.me(100,0,0)&8)==0 && chain.t(101,0,0)==Block_Types::Stone);
  }
  puts("copper logic C++ smoke: OK (wire, walls, doors, observer, piston, sticky, chunk boundary)");
}
