// Standalone smoke test: g++ -std=c++20 -Isrc tools/tests/mob_hit_zone_smoke.cpp -o /tmp/mob_hit_zone_smoke
#include "mobs/Mob_Hit_Zone.h"
#include <cassert>
#include <string>

int main() {
    assert(mob_hit_zone_damage_multiplier(Mob_Hit_Zone::Torso) == 0.8f);
    assert(mob_hit_zone_damage_multiplier(Mob_Hit_Zone::Arm) == 1.0f);
    assert(mob_hit_zone_damage_multiplier(Mob_Hit_Zone::Leg) == 1.2f);
    assert(mob_hit_zone_damage_multiplier(Mob_Hit_Zone::Head) == 1.8f);
    assert(std::string(mob_hit_zone_name(Mob_Hit_Zone::Head)) == "Head");
    assert(mob_ray_aabb({0,1,0}, {1,0,0}, {2,0.5f,-0.5f}, {3,1.5f,0.5f}, 5) == 2.0f);
    assert(mob_ray_aabb({0,1,0}, {1,0,0}, {2,3,-0.5f}, {3,4,0.5f}, 5) < 0.0f);
    assert(mob_ray_aabb({0,1,0}, {0,1,0}, {2,0.5f,-0.5f}, {3,1.5f,0.5f}, 5) < 0.0f);
    return 0;
}
