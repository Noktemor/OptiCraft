// Tests the real Structure_Placement cache with a flat world and no river model.
#include "world/generation/Structure_Placement.h"
#include "world/generation/Village_Layout.h"
#include "world/generation/River_Network.h"
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

// This narrow, test-only stub avoids constructing the full river hydrology model.
River_Network::Column River_Network::column_at(int, int) const { return {}; }

int main() {
    Structure_Placement placement(1327, [](int,int) { return 83; }, nullptr, 63);
    int found=0, roads=0;
    for (int cx=-5;cx<=5;++cx)
        for(int cz=-5;cz<=5;++cz) {
            const auto site=placement.village_in_cell(cx,cz);
            const auto plan=placement.village_plan_in_cell(cx,cz);
            assert(bool(site)==bool(plan));
            if(!site)continue;
            assert(site->radius>=Structure_Placement::kRadiusMin && site->radius<=Structure_Placement::kRadiusMax);
            assert(placement.village_center_at(site->x,site->z));
            assert(plan==placement.village_plan_in_cell(cx,cz));
            assert(!plan->buildings.empty());
            roads+=static_cast<int>(plan->roads.size());
            ++found;
        }
    assert(found>20 && roads>1000);
    std::vector<std::thread> threads;
    for (int i=0;i<8;++i)
        threads.emplace_back([&] {
            for (int n=0;n<50;++n) {
                const auto a=placement.village_plan_in_cell(-3,2);
                const auto b=placement.village_plan_in_cell(-3,2);
                assert(a==b);
            }
        });
    for(auto& t:threads)t.join();
    placement.clear_cache();
    // Same world coordinates remain reproducible after eviction/reset.
    auto a=placement.village_plan_in_cell(0,0);
    auto b=placement.village_plan_in_cell(0,0);
    assert(a==b);
    std::cout<<"structure placement smoke passed: "<<found<<" sites, "<<roads
             <<" road samples, negative coords and 8 threads\n";
}
