// Bosses share spawn, targeting, hitboxes and HUD reporting. Knight keeps its
// dedicated Knight_Ai implementation; the two princesses use this class.
#ifndef OPTICRAFT_BOSS_H
#define OPTICRAFT_BOSS_H

#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include "mobs/Mob.h"
#include "mobs/Celestial_Boss_Rules.h"
#include "mobs/Boss_Choreography.h"

class Chunk_Manager;

// Existing numeric IDs remain stable.
enum class Boss_Kind : uint8_t {
    Stone_Colossus = 0,
    Shadow_Stalker,
    Knight,
    Moon_Princess,
    Sun_Princess,
    Count
};

struct Boss_Stats {
    const char* label;
    const char* model_path;
    float max_health;
    float scale;
};
const Boss_Stats& boss_stats(Boss_Kind kind);

struct Boss_Player_View {
    glm::vec3 position{0.0f};
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
    bool alive = true;
};
struct Boss_Tick_Result { int player_damage = 0; };

class Boss {
public:
    Boss(Boss_Kind kind, std::string name, const glm::vec3& spawn_position);
    bool load_model();
    Boss_Tick_Result update(float dt, const Boss_Player_View& player,
                            Chunk_Manager* chunks = nullptr, Mob_Nav::System* navigation = nullptr);
    void render(const Shader& shader, const glm::mat4& view, const glm::mat4& projection, float ambient) const;

    Boss_Kind kind() const { return m_kind; }
    const std::string& name() const { return m_name; }
    const glm::vec3& get_position() const { return m_position; }
    float get_health() const { return m_health; }
    float get_max_health() const { return boss_stats(m_kind).max_health; }
    int active_parts() const { return Celestial_Boss_Rules::alive_count(m_part_health); }
    Boss_Choreography::Sun_Move sun_technique() const { return m_sun_move; }
    const char* sun_technique_name() const;
    bool second_phase() const { return m_kind == Boss_Kind::Sun_Princess && Celestial_Boss_Rules::sun_second_phase(m_part_health); }
    float part_health(int index) const { return index >= 0 && index < 4 ? m_part_health[index] : 0.0f; }

    void compute_hitbox(glm::vec3& out_min, glm::vec3& out_max) const;
    Mob_Hit_Result raycast_body_part(const glm::vec3& eye, const glm::vec3& direction, float reach) const;
    // satellite index 0..3, -1 = core. Moon core invulnerable while any shield survives.
    bool take_damage(float amount, const glm::vec3& attacker_position, int satellite = -1);
    bool kill();
    bool is_dying() const { return m_dying; }
    bool is_removable() const { return m_dying && m_death_timer >= 3.0f; }

private:
    struct Wind_Blade {
        glm::vec3 position{0.0f};
        glm::vec3 direction{0.0f, 0.0f, -1.0f};
        float lifetime = 0.0f;
        Mob visual;
    };

    Boss_Kind m_kind;
    std::string m_name;
    Mob m_mob;
    glm::vec3 m_position;
    float m_health;
    bool m_dying = false;
    float m_death_timer = 0.0f;
    float m_hurt_timer = 0.0f;
    float m_timer = 0.0f;
    int m_phase = 0;

    std::array<float, 4> m_part_health{};
    std::array<float, 4> m_part_flash{};
    std::array<Mob, 4> m_satellites;
    float m_orbit_time = 0.0f;
    float m_attack_timer = 0.0f;
    // Sun's scripted technique timeline, not a generic attack cooldown.
    Boss_Choreography::Sun_Move m_sun_move = Boss_Choreography::Sun_Move::Idle;
    Boss_Choreography::Sun_Move m_sun_previous = Boss_Choreography::Sun_Move::Idle;
    float m_sun_timeline = 0.0f;
    unsigned m_sun_sequence = 0;
    unsigned m_moon_attack_sequence = 0;
    unsigned m_generic_combo_sequence = 0;
    unsigned m_generic_combo = 0;
    unsigned m_generic_hit_mask = 0;
    glm::vec3 m_sun_focus{0.0f};
    int m_sun_active_hand = -1;
    std::vector<Wind_Blade> m_blades;
    Mob_Nav::Agent m_flight_agent;
    glm::vec3 satellite_position(int index) const;
    void update_satellites(float dt);
    void fly_towards(float dt, const glm::vec3& target, float speed,
                     Chunk_Manager* chunks, Mob_Nav::System* navigation);
    void face_towards(const glm::vec3& target);
    Boss_Tick_Result update_stone_colossus(float dt, const Boss_Player_View& player, Chunk_Manager* chunks);
    Boss_Tick_Result update_shadow_stalker(float dt, const Boss_Player_View& player, Chunk_Manager* chunks);
    Boss_Tick_Result update_moon(float dt, const Boss_Player_View& player,
                                 Chunk_Manager* chunks, Mob_Nav::System* navigation);
    Boss_Tick_Result update_sun(float dt, const Boss_Player_View& player,
                                Chunk_Manager* chunks, Mob_Nav::System* navigation);
};
#endif
