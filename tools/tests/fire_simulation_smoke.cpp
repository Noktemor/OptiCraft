// g++ -std=c++20 -O2 -Itools/tests/nether_smoke/stubs -Isrc \
//     tools/tests/fire_simulation_smoke.cpp -o /tmp/fire_simulation_smoke
#include "world/logic/Fire_Simulation.h"
#include <cassert>
#include <sstream>
int main(){
 Chunk_Manager world;
 Fire_Simulation fire;
 world.set_block_world(0,0,0,Block_Types::Stone);
 assert(fire.ignite({0,1,0},world));
 assert(world.get_block_world(0,1,0)==Block_Types::Fire);
 assert(!fire.ignite({0,1,0},world));
 assert(!fire.ignite({2,1,0},world)); // no support
 std::stringstream persistence;
 fire.save(persistence);
 Fire_Simulation loaded;
 loaded.load(persistence);
 int visits=0;
 loaded.for_each_near({0,1,0},3.f,8,[&](const glm::vec3&){++visits;});
 assert(visits==1);
 for(int i=0;i<100;++i) loaded.tick(1.f,world);
 assert(world.get_block_world(0,1,0)==Block_Types::Air);
 return 0;
}
