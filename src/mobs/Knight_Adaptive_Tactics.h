#ifndef OPTICRAFT_KNIGHT_ADAPTIVE_TACTICS_H
#define OPTICRAFT_KNIGHT_ADAPTIVE_TACTICS_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// Personal, short-term combat memory of ONE knight against its current target.
// Tracks observed dodge timing and outcomes, not hidden player inputs.
// Deliberate timing changes are earned by repeated evidence.
class Knight_Adaptive_Tactics {
public:
    void begin_windup() { m_dodge_observed_this_attack = false; }
    void observe_dodge(float windup_elapsed, float tangent_speed, float radial_speed) {
        if (m_dodge_observed_this_attack || windup_elapsed < .10f || windup_elapsed > 1.3f) return;
        // Sudden lateral motion or a rapid retreat during a visible tell.
        if (std::abs(tangent_speed) < 4.1f && radial_speed < 4.1f) return;
        m_dodge_observed_this_attack = true;
        m_dodge_events = std::min(30, m_dodge_events+1);
        m_dodge_confidence = std::min(12.0f,m_dodge_confidence+1.0f);
        const float w = m_dodge_events <= 4 ? 1.0f / m_dodge_events : .24f;
        m_typical_dodge_time += (windup_elapsed - m_typical_dodge_time) * w;
    }
    float intentional_delay(uint32_t choice) const {
        if (m_dodge_events < 3 || m_dodge_confidence < 2.5f || choice % 3 == 0) return 0.0f;
        return std::clamp(.20f + .2f * m_typical_dodge_time, .20f, .38f);
    }
    // Compose an opener->finisher suggestion from learned *player* behavior,
    // attack results and boss phase. Attack IDs match Knight_Attack in Knight_Ai.h.
    // No on-the-fly animation generation, only authored valid transitions.
    int propose_followup(int last_attack, int player_pattern, bool connected, int phase) const {
        int followup = 0;
        switch (player_pattern) {
            case 1: case 2: case 3: // rear flank / orbit
                followup = phase >= 3 ? 7 : 3; break;
            case 4: // kiting
                followup = phase >= 2 ? 4 : 2; break;
            case 5: // repeated jumps
                followup = phase >= 3 ? 6 : 3; break;
            case 6: // melee spamming
                followup = phase >= 3 ? 7 : 2; break;
            case 7: // rushing
                followup = phase >= 3 ? 8 : 3; break;
            default:
                if (m_dodge_confidence >= 2.5f && !connected) followup = 2;
                else followup = last_attack == 1 ? 2 : (last_attack == 2 ? 3 : 1);
                break;
        }
        if (followup == last_attack) followup = last_attack == 1 ? 2 : 1;
        return followup;
    }
    void complete_attack(int index, bool hit) {
        if (index < 1 || index >= static_cast<int>(m_attempts.size())) return;
        m_attempts[index] = std::min(40.0f, m_attempts[index] + 1.0f);
        if (hit) m_hits[index] = std::min(40.0f, m_hits[index] + 1.0f);
        if (m_attempts[index] >= 39.9f) {
            m_attempts[index] *= .75f;
            m_hits[index] *= .75f;
        }
    }
    float preference(int index) const {
        if (index < 1 || index >= static_cast<int>(m_attempts.size())) return 1.0f;
        if (m_attempts[index] < 2.0f) return 1.0f;
        const float smoothed_hit_rate = (m_hits[index] + 1.0f) / (m_attempts[index] + 2.0f);
        return .70f + 1.10f * smoothed_hit_rate;
    }
    void decay(float dt) {
        const float scale = std::exp(-.69314718f * std::max(0.0f, dt) / 210.0f);
        for (float& v: m_attempts) v *= scale;
        for (float& v: m_hits) v *= scale;
        m_dodge_confidence *= scale;
    }
    int dodge_events() const { return m_dodge_events; }
    float expected_dodge_time() const { return m_typical_dodge_time; }
private:
    std::array<float,9> m_attempts{};
    std::array<float,9> m_hits{};
    float m_typical_dodge_time = .25f;
    float m_dodge_confidence = 0.0f;
    int m_dodge_events = 0;
    bool m_dodge_observed_this_attack = false;
};
#endif
