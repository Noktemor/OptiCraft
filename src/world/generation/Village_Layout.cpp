#include "world/generation/Village_Layout.h"
#include "world/generation/Structure_Placement.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace {
uint64_t mix(uint64_t v) {
    v += 0x9E3779B97F4A7C15ull;
    v = (v ^ (v >> 30)) * 0xBF58476D1CE4E5B9ull;
    v = (v ^ (v >> 27)) * 0x94D049BB133111EBull;
    return v ^ (v >> 31);
}
uint64_t random_at(uint32_t seed, int x, int z, int salt) {
    return mix(mix(seed ^ 0x56494C4C414745ull) ^
        mix(static_cast<uint32_t>(x)) ^ (mix(static_cast<uint32_t>(z)) << 1) ^
        static_cast<uint32_t>(salt));
}
int face_towards(int dx, int dz) {
    if (dz < 0) return 0;
    if (dx > 0) return 1;
    if (dz > 0) return 2;
    return 3;
}
}

Village_Plan make_village_plan(uint32_t seed, const Village_Site& site,
                               const std::function<int(int,int)>& height) {
    Village_Plan plan;
    plan.min_x = site.x - site.radius - 12;
    plan.max_x = site.x + site.radius + 12;
    plan.min_z = site.z - site.radius - 12;
    plan.max_z = site.z + site.radius + 12;
    const int center_y = height(site.x, site.z);
    plan.min_y = center_y - 16;
    plan.max_y = center_y + 30;
    plan.buildings.push_back({Structure_Template_Id::Well, site.x, center_y, site.z, 0});

    // Four orthogonal streets. Slight bends are deterministic and do not require
    // neighbourhood worker synchronization; each road tile stores its own ground Y.
    constexpr int dirs[4][2] = {{1,0}, {-1,0}, {0,1}, {0,-1}};
    for (int street = 0; street < 4; ++street) {
        const int dx = dirs[street][0], dz = dirs[street][1];
        const uint64_t h = random_at(seed, site.x, site.z, street + 100);
        const int length = std::min(site.radius - 12,
                                    29 + static_cast<int>((h >> 11) % static_cast<uint64_t>(std::max(1, site.radius - 37))));
        const int bend = static_cast<int>((h >> 35) % 7) - 3;
        for (int d = 0; d <= length; ++d) {
            const int lateral = (d > 25) ? bend * (d - 25) / std::max(1, length - 25) : 0;
            const int x = site.x + dx * d - dz * lateral;
            const int z = site.z + dz * d + dx * lateral;
            const int y = height(x, z);
            if (y < 65 || std::abs(y - center_y) > 10) continue;
            plan.roads.push_back({x, y, z});
        }
        // House candidate: face normal points back to the road (not along it).
        for (int d = 16; d + 11 < length; d += 19) {
            for (int side : {-1, 1}) {
                const int side_x = -dz * side;
                const int side_z = dx * side;
                const int lateral = (d > 25) ? bend * (d - 25) / std::max(1, length - 25) : 0;
                const int door_x = site.x + dx * d - dz * lateral + side_x * 5;
                const int door_z = site.z + dz * d + dx * lateral + side_z * 5;
                const int y = height(door_x, door_z);
                if (y < 66 || std::abs(y - center_y) > 7) continue;
                // Avoid cliffs; sample actual house footprint corners in world space.
                const int back_x = door_x + side_x * 9;
                const int back_z = door_z + side_z * 9;
                const int width_x = dx * 5, width_z = dz * 5;
                const int heights[4] = {
                    height(door_x + width_x, door_z + width_z),
                    height(door_x - width_x, door_z - width_z),
                    height(back_x + width_x, back_z + width_z),
                    height(back_x - width_x, back_z - width_z)
                };
                const int lowest = *std::min_element(std::begin(heights), std::end(heights));
                const int highest = *std::max_element(std::begin(heights), std::end(heights));
                if (highest - lowest > 3 || std::abs(highest - y) > 3) continue;
                const auto roll = random_at(seed, door_x, door_z, 0xC011);
                const Structure_Template_Id id = (roll % 11 == 0) ? Structure_Template_Id::Farm :
                    (roll % 2 == 0) ? Structure_Template_Id::Cottage_Birch : Structure_Template_Id::Cottage_Oak;
                const int rotation = face_towards(-side_x, -side_z);
                plan.buildings.push_back({id, door_x, highest, door_z,
                                           static_cast<uint8_t>(rotation)});
            }
        }
    }
    return plan;
}
