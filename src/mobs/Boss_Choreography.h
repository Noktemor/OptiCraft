#ifndef OPTICRAFT_BOSS_CHOREOGRAPHY_H
#define OPTICRAFT_BOSS_CHOREOGRAPHY_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// Data-driven windows shared by the celestial bosses. No GL, allocator or
// world access. The AI chooses a *sequence* of authored motions rather than
// random frame-by-frame attacks; collision is checked separately in Boss.cpp.
namespace Boss_Choreography {

enum class Sun_Move : uint8_t {
    Idle, Palm_Lance, Twin_Seal, Halo_Sweep, Sun_Dash,
    Air_Step, Rising_Kick, Crown_Dance, Falling_Star, Delayed_Palm
};
struct Move_Spec {
    float tell;       // readable, non-damaging anticipation
    float active;     // hitboxes only activate in this window
    float recovery;   // guaranteed opportunity to punish the boss
    float range;
    int damage;
    std::array<float,3> strikes; // seconds from the beginning of active, -1 = unused
};
inline Move_Spec sun_spec(Sun_Move move) {
    switch (move) {
        case Sun_Move::Palm_Lance:  return {.55f,.62f,.48f,5.4f,5,{.16f,-1,-1}};
        case Sun_Move::Twin_Seal:   return {.65f,.93f,.56f,5.4f,5,{.19f,.66f,-1}};
        case Sun_Move::Halo_Sweep:  return {.82f,1.07f,.68f,5.1f,4,{.21f,.52f,.88f}};
        case Sun_Move::Sun_Dash:    return {.68f,.55f,.65f,3.3f,7,{.28f,-1,-1}};
        case Sun_Move::Air_Step:    return {.46f,.63f,.47f,3.4f,8,{.26f,-1,-1}};
        case Sun_Move::Rising_Kick: return {.71f,.77f,.65f,3.2f,10,{.34f,-1,-1}};
        case Sun_Move::Crown_Dance: return {.86f,1.13f,.74f,3.5f,6,{.20f,.58f,.96f}};
        case Sun_Move::Falling_Star:return {1.05f,.68f,.95f,4.2f,14,{.46f,-1,-1}};
        case Sun_Move::Delayed_Palm:return {.97f,.66f,.75f,3.0f,12,{.36f,-1,-1}};
        default: return {0,0,0,0,0,{-1,-1,-1}};
    }
}
inline bool uses_hands(Sun_Move move) {
    return move == Sun_Move::Palm_Lance || move == Sun_Move::Twin_Seal || move == Sun_Move::Halo_Sweep;
}
inline bool in_strike_window(const Move_Spec& spec, int index, float previous_t, float current_t) {
    if (index < 0 || index >= 3 || spec.strikes[index] < 0.0f) return false;
    const float hit_at = spec.tell + spec.strikes[index];
    return previous_t < hit_at && current_t >= hit_at;
}
inline float strike_pulse(float current_t, float hit_t, float width = .31f) {
    return std::clamp(1.0f - std::abs(current_t - hit_t) / width, 0.0f, 1.0f);
}
// A limited combo graph. Losing hands unlocks movement variants but does not
// make her core invulnerable; with all four hands she can still be killed.
inline Sun_Move choose_sun_move(int hands, float distance, Sun_Move last, unsigned sequence) {
    if (hands > 0) {
        if (distance > 9.0f) return Sun_Move::Sun_Dash;
        constexpr std::array<Sun_Move,5> pattern = {
            Sun_Move::Palm_Lance, Sun_Move::Twin_Seal, Sun_Move::Air_Step,
            Sun_Move::Halo_Sweep, Sun_Move::Sun_Dash
        };
        Sun_Move next = pattern[sequence % pattern.size()];
        if (hands == 1 && next == Sun_Move::Twin_Seal) next = Sun_Move::Palm_Lance;
        if (hands < 3 && next == Sun_Move::Halo_Sweep) next = Sun_Move::Air_Step;
        if (next == last) next = Sun_Move::Air_Step;
        return next;
    }
    if (distance > 8.0f) return Sun_Move::Sun_Dash;
    constexpr std::array<Sun_Move,5> pattern = {
        Sun_Move::Air_Step, Sun_Move::Rising_Kick,
        Sun_Move::Crown_Dance, Sun_Move::Delayed_Palm, Sun_Move::Falling_Star
    };
    Sun_Move next = pattern[sequence % pattern.size()];
    return next == last ? Sun_Move::Rising_Kick : next;
}
inline bool has_hitbox(Sun_Move move, int hands) {
    return move != Sun_Move::Idle && (!uses_hands(move) || hands > 0);
}
} // namespace Boss_Choreography
#endif
