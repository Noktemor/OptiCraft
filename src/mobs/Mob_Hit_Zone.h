// Damage regions are distinct from physical collision AABB (Config::*_width/height).
#ifndef OPTICRAFT_MOB_HIT_ZONE_H
#define OPTICRAFT_MOB_HIT_ZONE_H

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>

// The first named hit region crossed by the attack ray decides the damage.
enum class Mob_Hit_Zone : uint8_t { Torso, Arm, Leg, Head };

inline const char* mob_hit_zone_name(Mob_Hit_Zone zone) {
    switch (zone) {
        case Mob_Hit_Zone::Head: return "Head";
        case Mob_Hit_Zone::Leg: return "Leg";
        case Mob_Hit_Zone::Arm: return "Arm";
        default: return "Torso";
    }
}

// Deliberately: torso < legs < head. Tunable independently of weapon strength.
inline float mob_hit_zone_damage_multiplier(Mob_Hit_Zone zone) {
    switch (zone) {
        case Mob_Hit_Zone::Head: return 1.8f;
        case Mob_Hit_Zone::Leg: return 1.2f;
        case Mob_Hit_Zone::Arm: return 1.0f;
        default: return 0.8f;
    }
}

struct Mob_Hit_Result {
    float distance = -1.0f;
    Mob_Hit_Zone zone = Mob_Hit_Zone::Torso;
    int subpart = -1; // Optional independent target (boss shield/hand); -1 means core.
    explicit operator bool() const { return distance >= 0.0f; }
};

inline float mob_ray_aabb(const glm::vec3& origin, const glm::vec3& dir,
                          const glm::vec3& lo, const glm::vec3& hi, float reach) {
    float near = 0.0f;
    float far = reach;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(dir[axis]) < 1e-6f) {
            if (origin[axis] < lo[axis] || origin[axis] > hi[axis]) return -1.0f;
        } else {
            const float a = (lo[axis] - origin[axis]) / dir[axis];
            const float b = (hi[axis] - origin[axis]) / dir[axis];
            near = std::max(near, std::min(a, b));
            far = std::min(far, std::max(a, b));
            if (near > far) return -1.0f;
        }
    }
    return near;
}
#endif // OPTICRAFT_MOB_HIT_ZONE_H
