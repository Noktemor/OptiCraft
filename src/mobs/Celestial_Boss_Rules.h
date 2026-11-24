#ifndef OPTICRAFT_CELESTIAL_BOSS_RULES_H
#define OPTICRAFT_CELESTIAL_BOSS_RULES_H

#include <algorithm>
#include <array>
#include <cmath>

// Pure rules, shared by game logic and standalone tests. Core HP is separate from
// satellite HP: shields/hands cannot accidentally reduce the princess' health.
namespace Celestial_Boss_Rules {
constexpr float MoonHealth = 300.0f;
constexpr float SunHealth = 250.0f;
constexpr float MoonShieldHealth = 45.0f;
constexpr float SunHandHealth = 70.0f;
constexpr int SatelliteCount = 4;

inline int alive_count(const std::array<float, SatelliteCount>& parts) {
    return static_cast<int>(std::count_if(parts.begin(), parts.end(), [](float hp) { return hp > 0.0f; }));
}
inline bool damage_satellite(std::array<float, SatelliteCount>& parts, int index, float damage) {
    if (index < 0 || index >= SatelliteCount || parts[index] <= 0.0f || damage <= 0.0f) return false;
    parts[index] = std::max(0.0f, parts[index] - damage);
    return true;
}
// Three seconds near ground per eight-second cycle; shields are hittable with
// an ordinary melee weapon even without bows or other ranged equipment.
inline bool moon_swooping(float elapsed_seconds) {
    return std::fmod(std::max(elapsed_seconds, 0.0f), 8.0f) >= 5.0f;
}
inline bool moon_vulnerable(const std::array<float, SatelliteCount>& parts) {
    return alive_count(parts) == 0;
}
inline bool sun_second_phase(const std::array<float, SatelliteCount>& parts) {
    return alive_count(parts) == 0;
}
inline float sun_speed(const std::array<float, SatelliteCount>& parts) {
    return 2.4f + 0.9f * static_cast<float>(SatelliteCount - alive_count(parts));
}
inline float sun_attack_interval(const std::array<float, SatelliteCount>& parts) {
    return 2.1f - 0.24f * static_cast<float>(SatelliteCount - alive_count(parts));
}
}  // namespace Celestial_Boss_Rules
#endif
