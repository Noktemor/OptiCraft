#include "world/generation/Structure_Pack.h"
#include "world/generation/Village_Layout.h"
#include "world/generation/Structure_Placement.h"
#include "world/generation/Structure_Adapters.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <map>
#include <set>
#include <thread>
#include <tuple>
#include <vector>

using Point = std::tuple<int,int,int>;
using World = std::map<Point, std::pair<Block_Types,uint8_t>>;

uint64_t plan_hash(const Village_Plan& p) {
    uint64_t h = 14695981039346656037ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    for (auto& r : p.roads) {mix(static_cast<uint32_t>(r.x));mix(static_cast<uint32_t>(r.y));mix(static_cast<uint32_t>(r.z));}
    for (auto& b : p.buildings) {mix(static_cast<uint32_t>(b.entrance_world_x));mix(static_cast<uint32_t>(b.base_world_y));mix(static_cast<uint32_t>(b.entrance_world_z));mix(b.rotation);mix(static_cast<uint8_t>(b.template_id));}
    return h;
}

void paint(const Structure_Template& t, const Structure_Instance& inst, World& out,
           int minx, int maxx, int miny, int maxy, int minz, int maxz) {
    for (const auto& b : t.voxels) {
        int x,y,z;
        Structure_Pack::world_position(t,inst,b,x,y,z);
        if (x < minx || x > maxx || y < miny || y > maxy || z < minz || z > maxz) continue;
        const uint8_t meta = (b.flags & 1u) ? static_cast<uint8_t>(
            (b.meta & 0xFCu) | ((b.meta+inst.rotation) & 3u)) : b.meta;
        out[{x,y,z}] = {b.type,meta};
    }
}

int main() {
    const auto& pack = Structure_Pack::instance();
    assert(pack.ready());
    const auto* oak = pack.get(Structure_Template_Id::Cottage_Oak);
    const auto* birch = pack.get(Structure_Template_Id::Cottage_Birch);
    const auto* fortress = pack.get(Structure_Template_Id::Fortress);
    assert(oak && birch && fortress);
    assert(oak->voxels.size() > 700 && fortress->width == 65 && fortress->depth == 65);

    // Door: two halves with correct original and rotated facing bits.
    const Structure_Instance inst{Structure_Template_Id::Cottage_Oak,31,83,31,1};
    World whole;
    paint(*oak,inst,whole,-100,150,0,160,-100,150);
    assert((whole[{31,84,31}].first == static_cast<Block_Types>(299)));
    assert((whole[{31,84,31}].second == 1));
    assert((whole[{31,85,31}].second == 9));

    // Splitting into chunks (including Y=64/96 boundary) must match full placement.
    World clipped;
    for (int cy=1; cy<=3; ++cy)
      for (int cz=-1; cz<=2; ++cz)
        for (int cx=-1; cx<=2; ++cx)
            paint(*oak,inst,clipped,cx*32,cx*32+31,cy*32,cy*32+31,cz*32,cz*32+31);
    assert(whole == clipped);

    // Large monolithic structure can be placed with the SAME single-instance path.
    World castle;
    Structure_Instance big{Structure_Template_Id::Fortress,31,85,31,2};
    paint(*fortress,big,castle,-100,150,0,200,-100,150);
    assert(castle.size() > 70000);
    World castle_parts;
    for (int cy=1; cy<=4; ++cy)
      for (int cz=-2; cz<=3; ++cz)
        for (int cx=-2; cx<=3; ++cx)
            paint(*fortress,big,castle_parts,cx*32,cx*32+31,cy*32,cy*32+31,cz*32,cz*32+31);
    assert(castle == castle_parts);

    const auto flat = [](int,int) {return 81;};
    Village_Site village{31,-23,110};
    const auto base=make_village_plan(1327,village,flat);
    assert(base.roads.size()>100 && base.buildings.size()>=5);
    assert(base.buildings.front().template_id==Structure_Template_Id::Well);
    const auto hash=plan_hash(base);
    assert(hash==plan_hash(make_village_plan(1327,village,flat)));
    assert(hash!=plan_hash(make_village_plan(1328,village,flat)));
    assert(plan_hash(make_village_plan(1327,Village_Site{-301,-287,110},flat))==
        plan_hash(make_village_plan(1327,Village_Site{-301,-287,110},flat)));
    std::vector<std::thread> workers;
    for (int i=0; i<8; ++i) workers.emplace_back([&] {
        for (int n=0;n<20;++n) {
            assert(pack.get(Structure_Template_Id::Cottage_Oak)==oak);
            assert(plan_hash(make_village_plan(1327,village,flat))==hash);
        }
    });
    for (auto& t:workers) t.join();

    // Existing maze implementation is captured, not changed or rerun per chunk.
    Fast_Rng rng(52);
    Maze_Config config;
    config.min_area=config.max_area=70;
    config.min_floors=config.max_floors=3;
    const auto maze=Maze_Generator::generate(config,rng);
    assert(Maze_Generator::is_connected(maze));
    auto maze_template=make_maze_structure_template(maze);
    assert(!maze_template.voxels.empty());
    const auto d=make_dungeon_layout(45);
    auto dungeon_template=make_dungeon_structure_template(d);
    assert(!dungeon_template.voxels.empty());
    // A generated maze is a custom Structure_Instance, just like a packed house.
    Structure_Instance maze_instance;
    maze_instance.entrance_world_x=1024;maze_instance.base_world_y=50;
    maze_instance.entrance_world_z=-201;maze_instance.rotation=1;
    maze_instance.custom_template=std::make_shared<Structure_Template>(std::move(maze_template));
    World maze_blocks;
    paint(*maze_instance.custom_template,maze_instance,maze_blocks,-2000,2000,-250,1024,-2000,2000);
    assert(!maze_blocks.empty());

    std::cout << "village structure smoke passed: 5 templates, roads=" << base.roads.size()
              << ", buildings=" << base.buildings.size() << ", 8 threads, 65x65 fortress, maze adapter\n";
}
