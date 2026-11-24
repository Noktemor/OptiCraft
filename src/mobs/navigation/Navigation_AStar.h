#pragma once

// Pure grid-based path search. No GL, Chunk_Manager or GLM dependencies: safe for worker threads
// and usable by the standalone test runner.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <vector>

namespace Mob_Nav {

struct Cell {
    int x = 0, y = 0, z = 0;
    bool operator==(const Cell&) const = default;
};

enum class Travel : uint8_t { Ground, Flying };

struct Grid {
    int ox = 0, oy = 0, oz = 0;
    int sx = 0, sy = 0, sz = 0;
    // bit0 solid; bit1 liquid (slower); bit2 hazard (unwalkable)
    std::vector<uint8_t> voxels;

    bool inside(int x, int y, int z) const {
        return x >= ox && x < ox + sx && y >= oy && y < oy + sy && z >= oz && z < oz + sz;
    }
    int index(int x, int y, int z) const { return ((y - oy) * sz + (z - oz)) * sx + (x - ox); }
    Cell at(int i) const { return {ox + i % sx, oy + i / (sx * sz), oz + (i / sx) % sz}; }
    uint8_t flags(int x, int y, int z) const {
        return inside(x,y,z) ? voxels[static_cast<size_t>(index(x,y,z))] : uint8_t{1};
    }
    bool solid(int x, int y, int z) const { return (flags(x,y,z) & 1) != 0; }
    bool hazard(int x, int y, int z) const { return (flags(x,y,z) & 4) != 0; }
    bool clear(Cell feet, float radius, float height) const {
        const int xmin = static_cast<int>(std::floor(feet.x + 0.5f - radius + 0.001f));
        const int xmax = static_cast<int>(std::floor(feet.x + 0.5f + radius - 0.001f));
        const int zmin = static_cast<int>(std::floor(feet.z + 0.5f - radius + 0.001f));
        const int zmax = static_cast<int>(std::floor(feet.z + 0.5f + radius - 0.001f));
        const int ymax = static_cast<int>(std::floor(feet.y + height - 0.001f));
        for (int y = feet.y; y <= ymax; ++y)
            for (int z = zmin; z <= zmax; ++z)
                for (int x = xmin; x <= xmax; ++x)
                    if (solid(x,y,z) || hazard(x,y,z)) return false;
        return true;
    }
    bool supported(Cell feet, float radius) const {
        const int xmin = static_cast<int>(std::floor(feet.x + 0.5f - radius + 0.05f));
        const int xmax = static_cast<int>(std::floor(feet.x + 0.5f + radius - 0.05f));
        const int zmin = static_cast<int>(std::floor(feet.z + 0.5f - radius + 0.05f));
        const int zmax = static_cast<int>(std::floor(feet.z + 0.5f + radius - 0.05f));
        // Never treat the snapshot's out-of-bounds sentinel as actual ground.
        if (feet.y-1 < oy) return false;
        // At least one contact point can support the agent (allows stepping across overhangs).
        for (int z = zmin; z <= zmax; ++z)
            for (int x = xmin; x <= xmax; ++x)
                if (solid(x,feet.y-1,z)) return true;
        return false;
    }
    bool usable(Cell c, float radius, float height, Travel mode) const {
        return clear(c,radius,height) && (mode == Travel::Flying || supported(c,radius));
    }
};

struct Search_Settings {
    float radius = 0.35f;
    float height = 1.75f;
    Travel travel = Travel::Ground;
    int max_expansions = 10000;
};

struct Search_Result {
    std::vector<Cell> points;
    bool reached = false;
    int expansions = 0;
};

inline Search_Result search(const Grid& grid, Cell start, Cell goal, Search_Settings settings) {
    Search_Result result;
    if (grid.sx <= 0 || grid.sy <= 0 || grid.sz <= 0 || grid.voxels.size() !=
        static_cast<size_t>(grid.sx) * grid.sy * grid.sz) return result;
    const auto valid = [&](Cell c) { return grid.inside(c.x,c.y,c.z) && grid.usable(c,settings.radius,settings.height,settings.travel); };
    // Snap start/goal to a nearby standable position (mobs can currently be falling,
    // targets can stand on slabs/water, and the player's head is not the target feet).
    const auto snap = [&](Cell desired, int horizontal, int vertical, Cell& out) {
        float best = std::numeric_limits<float>::infinity();
        bool found = false;
        for (int dy = -vertical; dy <= vertical; ++dy)
            for (int dz = -horizontal; dz <= horizontal; ++dz)
                for (int dx = -horizontal; dx <= horizontal; ++dx) {
                    Cell c{desired.x+dx,desired.y+dy,desired.z+dz};
                    if (!valid(c)) continue;
                    const float score = static_cast<float>(dx*dx+dz*dz) + 1.8f*dy*dy;
                    if (score < best) { best = score; out = c; found = true; }
                }
        return found;
    };
    Cell first, last;
    if (!snap(start,1,3,first) || !snap(goal,3,4,last)) return result;
    const auto heuristic = [&](Cell a) {
        const float dx = static_cast<float>(a.x-last.x);
        const float dy = static_cast<float>(a.y-last.y);
        const float dz = static_cast<float>(a.z-last.z);
        return 10.0f * std::sqrt(dx*dx + dy*dy + dz*dz);
    };
    struct Open { float f; int index; bool operator<(const Open& other) const { return f > other.f; } };
    const int count = static_cast<int>(grid.voxels.size());
    std::vector<float> cost(static_cast<size_t>(count), std::numeric_limits<float>::infinity());
    std::vector<int> parent(static_cast<size_t>(count), -1);
    std::vector<uint8_t> closed(static_cast<size_t>(count), 0);
    std::priority_queue<Open> open;
    const int begin = grid.index(first.x,first.y,first.z);
    const int destination = grid.index(last.x,last.y,last.z);
    cost[begin] = 0.0f;
    open.push({heuristic(first),begin});
    int best = begin;
    float best_h = heuristic(first);
    const int limit = std::clamp(settings.max_expansions, 100, 30000);

    while (!open.empty() && result.expansions < limit) {
        const int id = open.top().index;
        open.pop();
        if (closed[id]) continue;
        closed[id] = 1;
        ++result.expansions;
        const Cell here = grid.at(id);
        const float h = heuristic(here);
        if (h < best_h) { best_h = h; best = id; }
        if (id == destination) { best = id; result.reached = true; break; }

        for (int dz=-1; dz<=1; ++dz) for (int dx=-1; dx<=1; ++dx) {
            if (!dx && !dz && settings.travel == Travel::Ground) continue;
            for (int dy = (settings.travel == Travel::Flying ? -1 : -2);
                 dy <= 1; ++dy) {
                if (settings.travel == Travel::Flying && !dx && !dy && !dz) continue;
                Cell next{here.x+dx,here.y+dy,here.z+dz};
                if (!valid(next)) continue;
                // No diagonal cuts through corners; for ground, check intermediate space
                // at the higher of the start/end feet elevations before passing sideways.
                const int high_y = std::max(here.y,next.y);
                if (dx && dz) {
                    if (!grid.clear({here.x+dx,high_y,here.z},settings.radius,settings.height) ||
                        !grid.clear({here.x,high_y,here.z+dz},settings.radius,settings.height)) continue;
                }
                if (settings.travel == Travel::Ground) {
                    // Head must clear the jump arc; don't climb straight through an overhang.
                    if (dy > 0 && !grid.clear({here.x,high_y,here.z},settings.radius,settings.height)) continue;
                    // An edge at the next cell shouldn't require walking through the wall.
                    if (dy == 0 && !grid.clear(next,settings.radius,settings.height)) continue;
                } else {
                    // Swept 3D corners: every intermediate axis/edge cell must fit the body.
                    // Checking only destination would let flying mobs slip through the corner
                    // between three mutually perpendicular walls.
                    if ((dx && !grid.clear({here.x+dx,here.y,here.z},settings.radius,settings.height)) ||
                        (dy && !grid.clear({here.x,here.y+dy,here.z},settings.radius,settings.height)) ||
                        (dz && !grid.clear({here.x,here.y,here.z+dz},settings.radius,settings.height)) ||
                        (dx && dy && !grid.clear({here.x+dx,here.y+dy,here.z},settings.radius,settings.height)) ||
                        (dy && dz && !grid.clear({here.x,here.y+dy,here.z+dz},settings.radius,settings.height)) ||
                        (dx && dz && !grid.clear({here.x+dx,here.y,here.z+dz},settings.radius,settings.height))) continue;
                }
                const int nid = grid.index(next.x,next.y,next.z);
                if (closed[nid]) continue;
                float step = 10.0f * std::sqrt(static_cast<float>(dx*dx + dy*dy + dz*dz));
                if (settings.travel == Travel::Ground) {
                    if (dy > 0) step += 7.0f;
                    if (dy < 0) step += 2.0f * -dy;
                }
                if (grid.flags(next.x,next.y,next.z) & 2) step *= 2.5f;
                const float tentative = cost[id] + step;
                if (tentative >= cost[nid]) continue;
                cost[nid] = tentative;
                parent[nid] = id;
                open.push({tentative+heuristic(next),nid});
            }
        }
    }
    // Return a useful partial route when the target is beyond this bounded snapshot.
    if (best == begin) return result;
    for (int i=best; i!=begin && i>=0; i=parent[i]) result.points.push_back(grid.at(i));
    std::reverse(result.points.begin(),result.points.end());
    // Only remove collinear cells, never corners or elevation changes.
    if (result.points.size() >= 3) {
        std::vector<Cell> compressed;
        compressed.reserve(result.points.size());
        compressed.push_back(result.points.front());
        for (size_t i=1; i+1<result.points.size(); ++i) {
            const Cell& a = compressed.back();
            const Cell& b = result.points[i];
            const Cell& c = result.points[i+1];
            const int dx1=b.x-a.x, dy1=b.y-a.y, dz1=b.z-a.z;
            const int dx2=c.x-b.x, dy2=c.y-b.y, dz2=c.z-b.z;
            if (dy1 != 0 || dy2 != 0 || dx1*dx2 + dz1*dz2 <= 0 ||
                (dx1 != 0 && dx2 != 0 && dx1*dz2 != dz1*dx2) ||
                (dx1 == 0 && dx2 != 0) || (dx1 != 0 && dx2 == 0) ||
                (dz1 == 0 && dz2 != 0) || (dz1 != 0 && dz2 == 0)) compressed.push_back(b);
        }
        compressed.push_back(result.points.back());
        result.points.swap(compressed);
    }
    return result;
}

} // namespace Mob_Nav
