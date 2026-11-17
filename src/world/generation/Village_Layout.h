#ifndef OPTICRAFT_VILLAGE_LAYOUT_H
#define OPTICRAFT_VILLAGE_LAYOUT_H

#include "world/generation/Structure_Pack.h"
#include <cstdint>
#include <functional>
#include <vector>

struct Village_Site;
struct Village_Road_Tile { int x = 0, y = 0, z = 0; };

// The layout is deterministic, immutable, and can be clipped independently by
// any X/Y/Z chunk without accessing neighbouring chunks or a mutable RNG.
struct Village_Plan {
    int min_x = 0, max_x = 0, min_y = 0, max_y = 0, min_z = 0, max_z = 0;
    std::vector<Village_Road_Tile> roads;
    std::vector<Structure_Instance> buildings;
};

Village_Plan make_village_plan(uint32_t seed, const Village_Site& site,
                               const std::function<int(int,int)>& get_height);

#endif
