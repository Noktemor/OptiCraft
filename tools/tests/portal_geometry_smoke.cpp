#include "world/portals/Portal_Placement.h"
#include <cassert>
#include <set>
#include <tuple>
#include <iostream>

using namespace Portal_Placement;

int main() {
    for (auto plane : {Plane::Along_X, Plane::Along_Z}) {
        std::set<std::tuple<int,int,int>> cells;
        int frames = 0, interiors = 0;
        for_each_cell(Kind::Nether, {-32,63,-17}, plane, [&](const Cell& c) {
            assert(c.pos.y >= 63 && c.pos.y <= 67);
            assert(!c.support);
            if (plane == Plane::Along_X) assert(c.pos.z == -17);
            else assert(c.pos.x == -32);
            ++(c.rim ? frames : interiors);
            assert(cells.insert({c.pos.x,c.pos.y,c.pos.z}).second);
        });
        assert(cells.size() == 20 && frames == 14 && interiors == 6);
    }
    {
        std::set<std::tuple<int,int,int>> cells;
        int frames = 0, interiors = 0, supports = 0;
        for_each_cell(Kind::End, {-15,62,-31}, Plane::Along_X, [&](const Cell& c) {
            assert(c.pos.y == (c.support ? 61 : 62));
            if (c.support) ++supports;
            else ++(c.rim ? frames : interiors);
            assert(cells.insert({c.pos.x,c.pos.y,c.pos.z}).second);
        });
        assert(cells.size() == 34 && frames == 16 && interiors == 9 && supports == 9);
    }
    std::cout << "portal geometry smoke: PASS\n";
}
