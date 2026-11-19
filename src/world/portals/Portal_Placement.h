// Deterministic portal geometry shared by commands and delayed portal placement.
// Header-only, no rendering or GL dependencies. Coordinates are world blocks.
#pragma once

namespace Portal_Placement {

enum class Kind { Nether, End };
enum class Plane { Along_X, Along_Z };

struct Position { int x = 0, y = 0, z = 0; };
struct Cell {
    Position pos;
    bool rim = false;
    bool support = false; // solid pad below End interior
};

// Visit each affected cell exactly once, including the End's support pad.
// Caller performs a readiness pass before writing any block, avoiding partial portals.
template<typename Visitor>
void for_each_cell(Kind kind, Position origin, Plane plane, Visitor&& visit) {
    if (kind == Kind::Nether) {
        for (int y = 0; y < 5; ++y) {
            for (int side = -1; side <= 2; ++side) {
                const bool rim = (y == 0 || y == 4 || side == -1 || side == 2);
                const Position pos = plane == Plane::Along_Z
                    ? Position{origin.x, origin.y + y, origin.z + side}
                    : Position{origin.x + side, origin.y + y, origin.z};
                visit(Cell{pos, rim, false});
            }
        }
        return;
    }
    for (int z = -2; z <= 2; ++z) {
        for (int x = -2; x <= 2; ++x) {
            const bool rim = x == -2 || x == 2 || z == -2 || z == 2;
            if (!rim) visit(Cell{{origin.x + x, origin.y - 1, origin.z + z}, false, true});
            visit(Cell{{origin.x + x, origin.y, origin.z + z}, rim, false});
        }
    }
}

} // namespace Portal_Placement
