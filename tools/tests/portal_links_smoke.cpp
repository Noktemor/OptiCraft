// Build: g++ -std=c++20 -Itools/tests/copper_logic_smoke/stubs -Isrc \
//        tools/tests/portal_links_smoke.cpp -o /tmp/portal_links_smoke
#include "world/portals/Portal_Link_Registry.h"
#include <cassert>
#include <sstream>
#include <cmath>
int main() {
    Portal_Link_Registry links;
    assert(links.remember({"overworld", {80,64,80}, false, false}));
    assert(!links.remember({"overworld", {80,64,80}, false, false}));
    assert(links.remember({"nether", {10,65,10}, true, false}));
    assert(links.remember({"overworld", {350,70,350}, false, false}));
    assert(!links.nearest("nether", 80.f, 80.f, false, 32.f));
    auto* site = links.nearest("nether", 10.f, 10.f, false, 32.f);
    assert(site && site->origin.y == 65 && site->along_z);
    const auto entrance = Portal_Link_Registry::entry_position(*site);
    assert(std::abs(entrance.x - 10.5f) < .001f && std::abs(entrance.y - 66.f) < .001f);
    assert(links.nearest("overworld",80.f,80.f,false,128.f)->origin.x == 80);
    assert(links.remember({"end", {8,70,8}, false, true}));
    assert(!links.nearest("end",8.f,8.f,false,128.f));
    std::stringstream file;
    links.save(file);
    Portal_Link_Registry recovered;
    recovered.load(file);
    assert(recovered.nearest("end",8.f,8.f,true,128.f));
    assert(recovered.forget({"nether", {10,65,10}, true, false}));
    assert(!recovered.nearest("nether",10.f,10.f,false,32.f));
    return 0;
}
