// Diagnostics for deterministic village sites and read-only layout cache.
// A diamond marker no longer exists: structures are placed in finish_generation.
#include "world/generation/World_Generator.h"
#include "world/generation/Structure_Placement.h"
#include "world/generation/Village_Layout.h"
#include "world/generation/Structure_Pack.h"
#include "world/generation/River_Network.h"

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    const uint32_t seed = argc > 1 ? static_cast<uint32_t>(std::strtoul(argv[1], nullptr, 10)) : 1327u;
    const int cells = argc > 2 ? std::atoi(argv[2]) : 16;
    World_Generator gen("Classic", seed, Dimension_Id::Overworld);
    River_Network rivers(seed, [&](int x,int z) { return gen.get_height(x,z); }, World_Generator::sea_level());
    Structure_Placement sites(seed, [&](int x,int z) { return gen.get_height(x,z); },
                              &rivers, World_Generator::sea_level());
    if (!Structure_Pack::instance().ready()) {
        std::fprintf(stderr, "assets/structures/structures.struct missing or invalid\n");
        return 2;
    }
    int total=0, roads=0, buildings=0;
    const int half=cells/2;
    for(int cz=-half;cz<half;++cz)
      for(int cx=-half;cx<half;++cx) {
        const auto site=sites.village_in_cell(cx,cz);
        if(!site) continue;
        const auto plan=sites.village_plan_in_cell(cx,cz);
        if (!plan) return 3;
        ++total;
        roads+=static_cast<int>(plan->roads.size());
        buildings+=static_cast<int>(plan->buildings.size());
        if(total<=8)
          std::printf("Village (%d,%d), radius %d, roads %zu, buildings %zu\n",
                      site->x,site->z,site->radius,plan->roads.size(),plan->buildings.size());
    }
    std::printf("Sites: %d, road nodes: %d, structure instances: %d\n",
                total,roads,buildings);
    return 0;
}
