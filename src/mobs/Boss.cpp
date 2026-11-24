#include "mobs/Boss.h"
#include "world/chunks/Chunk_Manager.h"
#include "world/blocks/Block_Types.h"
#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

const Boss_Stats& boss_stats(Boss_Kind kind) {
    static const Boss_Stats colossus{"Stone Colossus", "assets/mobs/zombie.glb", 200.0f, 2.5f};
    static const Boss_Stats stalker{"Shadow Stalker", "assets/mobs/skeleton.glb", 120.0f, 1.8f};
    static const Boss_Stats knight{"Knight", "assets/mobs/knight.glb", 1200.0f, 1.0f};
    static const Boss_Stats moon{"Принцесса Луны", "assets/mobs/moon_princess.glb", Celestial_Boss_Rules::MoonHealth, 1.0f};
    static const Boss_Stats sun{"Принцесса Солнца", "assets/mobs/sun_princess.glb", Celestial_Boss_Rules::SunHealth, 1.0f};
    switch (kind) {
        case Boss_Kind::Shadow_Stalker: return stalker;
        case Boss_Kind::Knight: return knight;
        case Boss_Kind::Moon_Princess: return moon;
        case Boss_Kind::Sun_Princess: return sun;
        default: return colossus;
    }
}

namespace {
    constexpr float kHurtFlashSeconds = 0.4f;
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kBladeSpeed = 11.0f;
    constexpr float kBladeLifetime = 3.0f;
    glm::vec3 unit_or(const glm::vec3& v, const glm::vec3& fallback) {
        const float d = glm::dot(v, v);
        return d < 1e-5f ? fallback : v / std::sqrt(d);
    }
    bool solid_at(const Chunk_Manager& chunks, const glm::vec3& p) {
        const glm::ivec3 c = glm::ivec3(glm::floor(p));
        return get_block_props(chunks.get_block_world(c.x,c.y,c.z)).is_solid;
    }
    // Rigid volume collision for short airborne dashes. Checking the entire
    // body prevents a fast AirStep from slipping through one-block walls.
    bool body_blocked(const Chunk_Manager& chunks, const glm::vec3& feet,
                      float half_width = .78f, float height = 3.45f) {
        const glm::ivec3 low = glm::ivec3(glm::floor(feet + glm::vec3(-half_width,.04f,-half_width)));
        const glm::ivec3 high = glm::ivec3(glm::floor(feet + glm::vec3(half_width,height,half_width)));
        for (int x=low.x; x<=high.x; ++x)
            for (int y=low.y; y<=high.y; ++y)
                for (int z=low.z; z<=high.z; ++z)
                    if (get_block_props(chunks.get_block_world(x,y,z)).is_solid) return true;
        return false;
    }
    // Sweep short steps so a fast projectile doesn't tunnel through voxel walls.
    bool blocked_segment(const Chunk_Manager& chunks, const glm::vec3& a, const glm::vec3& b) {
        const float len = glm::distance(a, b);
        const int steps = std::max(1, static_cast<int>(std::ceil(len / 0.25f)));
        for (int i = 1; i <= steps; ++i)
            if (solid_at(chunks, glm::mix(a,b,static_cast<float>(i)/steps))) return true;
        return false;
    }
}

const char* Boss::sun_technique_name() const {
    using Boss_Choreography::Sun_Move;
    switch (m_sun_move) {
        case Sun_Move::Palm_Lance: return "Palm Lance";
        case Sun_Move::Twin_Seal: return "Twin Seal";
        case Sun_Move::Halo_Sweep: return "Halo Sweep";
        case Sun_Move::Sun_Dash: return "Sun Dash";
        case Sun_Move::Air_Step: return "Air Step";
        case Sun_Move::Rising_Kick: return "Rising Kick";
        case Sun_Move::Crown_Dance: return "Crown Dance";
        case Sun_Move::Falling_Star: return "Falling Star";
        case Sun_Move::Delayed_Palm: return "Delayed Palm";
        default: return "Idle";
    }
}

Boss::Boss(Boss_Kind kind, std::string name, const glm::vec3& spawn_position)
    : m_kind(kind), m_name(std::move(name)), m_position(spawn_position),
      m_health(boss_stats(kind).max_health) {
    m_mob.set_position(spawn_position);
    m_mob.set_scale(boss_stats(kind).scale);
    if (kind == Boss_Kind::Moon_Princess) m_part_health.fill(Celestial_Boss_Rules::MoonShieldHealth);
    if (kind == Boss_Kind::Sun_Princess) m_part_health.fill(Celestial_Boss_Rules::SunHandHealth);
    if (kind == Boss_Kind::Moon_Princess || kind == Boss_Kind::Sun_Princess)
        m_mob.set_movement_mode(Mob::Movement_Mode::Flying);
}

bool Boss::load_model() {
    if (!m_mob.load(boss_stats(m_kind).model_path)) return false;
    if (m_kind == Boss_Kind::Moon_Princess && m_mob.has_animation("Moon_Idle"))
        m_mob.play_animation("Moon_Idle");
    else if (m_kind == Boss_Kind::Sun_Princess && m_mob.has_animation("Sun_Idle"))
        m_mob.play_animation("Sun_Idle");
    else if (m_mob.has_animation("Idle")) m_mob.play_animation("Idle");
    const char* part_path = m_kind == Boss_Kind::Moon_Princess ? "assets/mobs/moon_shield.glb" :
                            m_kind == Boss_Kind::Sun_Princess ? "assets/mobs/sun_hand.glb" : nullptr;
    if (part_path) {
        for (auto& part : m_satellites) if (!part.load(part_path)) return false;
        update_satellites(0.0f);
    }
    return true;
}

void Boss::face_towards(const glm::vec3& target) {
    glm::vec3 d = target - m_position;
    d.y = 0.0f;
    if (glm::length(d) < 0.001f) return;
    m_mob.set_yaw_degrees(glm::degrees(std::atan2(d.x, d.z)));
}

glm::vec3 Boss::satellite_position(int index) const {
    const float angle = m_orbit_time * (m_kind == Boss_Kind::Moon_Princess ? 0.9f : 1.35f)
                       + index * (kPi * 0.5f);
    const float radius = m_kind == Boss_Kind::Moon_Princess ? 1.65f : 1.85f;
    const glm::vec3 orbit = m_position + glm::vec3(std::cos(angle)*radius,
                                  1.8f + std::sin(m_orbit_time*2.0f + index)*0.28f,
                                  std::sin(angle)*radius);
    // During a palm strike, the *existing hand geometry* approaches its target,
    // then returns to orbit. The hitbox follows the same position as the mesh.
    if (m_kind == Boss_Kind::Sun_Princess && m_sun_active_hand == index &&
        Boss_Choreography::uses_hands(m_sun_move)) {
        const auto spec = Boss_Choreography::sun_spec(m_sun_move);
        float pulse = 0.0f;
        for (float strike : spec.strikes) if (strike >= 0.0f)
            pulse = std::max(pulse, Boss_Choreography::strike_pulse(m_sun_timeline,
                                                                  spec.tell + strike));
        return glm::mix(orbit, m_sun_focus + glm::vec3(0.0f,1.12f,0.0f), pulse * .87f);
    }
    return orbit;
}

void Boss::update_satellites(float dt) {
    m_orbit_time += dt;
    for (int i = 0; i < 4; ++i) {
        if (m_part_health[i] <= 0.0f) continue;
        m_part_flash[i] = std::max(0.0f, m_part_flash[i] - dt);
        m_satellites[i].set_hurt_flash(std::min(1.0f, m_part_flash[i] / kHurtFlashSeconds));
        m_satellites[i].set_position(satellite_position(i));
        m_satellites[i].set_yaw_degrees(-glm::degrees(m_orbit_time) * 0.7f);
        m_satellites[i].update(dt);
    }
}

void Boss::fly_towards(float dt, const glm::vec3& target, float speed,
                       Chunk_Manager* chunks, Mob_Nav::System* navigation) {
    // The navigator owns 3D path planning and checks the 3-block-tall body.
    // If no path system is passed (standalone AI tests), use a simple steering fallback.
    if (chunks && navigation) {
        m_mob.navigate_flying(dt, target, speed, 1.6f, 3.5f, m_flight_agent, *navigation, *chunks);
    } else {
        const glm::vec3 step = unit_or(target - m_position, glm::vec3(0.0f)) * speed * std::min(dt, 0.05f);
        m_mob.set_position(m_position + step);
    }
    m_position = m_mob.get_position();
}

Boss_Tick_Result Boss::update(float dt, const Boss_Player_View& player,
                              Chunk_Manager* chunks, Mob_Nav::System* navigation) {
    Boss_Tick_Result result;
    dt = std::clamp(dt, 0.0f, 0.05f);
    m_hurt_timer = std::max(0.0f, m_hurt_timer - dt);
    m_mob.set_hurt_flash(m_hurt_timer / kHurtFlashSeconds);
    if (m_dying) {
        m_death_timer += dt;
        m_mob.update(dt);
        return result;
    }
    if (player.alive) {
        switch (m_kind) {
            case Boss_Kind::Stone_Colossus: result = update_stone_colossus(dt, player, chunks); break;
            case Boss_Kind::Shadow_Stalker: result = update_shadow_stalker(dt, player, chunks); break;
            case Boss_Kind::Moon_Princess: result = update_moon(dt, player, chunks, navigation); break;
            case Boss_Kind::Sun_Princess: result = update_sun(dt, player, chunks, navigation); break;
            default: break;
        }
    }
    m_mob.set_position(m_position);
    m_mob.update(dt);
    if (m_kind == Boss_Kind::Moon_Princess || m_kind == Boss_Kind::Sun_Princess) update_satellites(dt);
    return result;
}

Boss_Tick_Result Boss::update_stone_colossus(float dt, const Boss_Player_View& player,
                                               Chunk_Manager* chunks) {
    Boss_Tick_Result result;
    const glm::vec3 aim = player.position + glm::vec3(0,1.1f,0);
    const glm::vec3 source = m_position + glm::vec3(0,2.5f,0);
    const float distance = glm::length(glm::vec2(player.position.x-m_position.x,
                                                 player.position.z-m_position.z));
    m_timer += dt;
    face_towards(player.position);
    const auto impact = [&](float range, int damage, unsigned bit, float instant) {
        if ((m_generic_hit_mask & bit) || m_timer < instant) return;
        m_generic_hit_mask |= bit;
        if (distance <= range && (!chunks || !blocked_segment(*chunks,source,aim)))
            result.player_damage += damage;
    };
    switch (m_phase) {
        case 0: // reposition and select a different authored heavy combination
            m_mob.set_roll_degrees(0.0f);
            if (distance > 4.3f) {
                const glm::vec3 towards = unit_or(glm::vec3(player.position.x-m_position.x,0,
                                                            player.position.z-m_position.z),glm::vec3(0));
                const glm::vec3 candidate = m_position + towards * (1.8f*dt);
                if (!chunks || !body_blocked(*chunks,candidate,1.52f,5.0f)) m_position = candidate;
                if (m_mob.has_animation("Walk")) m_mob.play_animation("Walk",true);
            } else {
                m_generic_combo = m_generic_combo_sequence++ % 3u;
                m_generic_hit_mask = 0;
                m_phase = 1; m_timer = 0.0f;
                if (m_mob.has_animation("Idle")) m_mob.play_animation("Idle",true);
            }
            break;
        case 1: { // distinct telegraphed windups
            const float windup = m_generic_combo==0 ? 1.05f : (m_generic_combo==1 ? .83f : 1.35f);
            m_mob.set_roll_pivot_height(2.0f);
            m_mob.set_roll_degrees((m_generic_combo==1 ? -1.0f : 1.0f) *
                                   14.0f * std::min(1.0f,m_timer/windup));
            if (m_timer >= windup) { m_phase=2; m_timer=0.0f; }
            break;
        }
        case 2: // slam / double sweep / wide tremor
            if (m_generic_combo==0) impact(4.6f,12,1u,.24f);
            else if (m_generic_combo==1) {
                impact(5.1f,7,1u,.17f);
                impact(5.1f,7,2u,.53f);
            } else impact(7.0f,9,1u,.35f);
            m_mob.set_roll_degrees(m_generic_combo==1 ?
                                   10.0f*std::sin(m_timer*8.0f) : -15.0f);
            if (m_timer >= (m_generic_combo==1 ? .70f : .57f)) {
                m_phase=3; m_timer=0.0f;
            }
            break;
        case 3: // punishable recovery
            m_mob.set_roll_degrees(0.0f);
            if (m_timer >= .95f) { m_phase=0; m_timer=0.0f; }
            break;
        default: m_phase=0; m_timer=0.0f; break;
    }
    return result;
}

Boss_Tick_Result Boss::update_shadow_stalker(float dt, const Boss_Player_View& player,
                                             Chunk_Manager* chunks) {
    Boss_Tick_Result result;
    m_timer += dt;
    const glm::vec3 towards = unit_or(glm::vec3(player.position.x-m_position.x,0,
                                                player.position.z-m_position.z),glm::vec3(0,0,1));
    const float distance = glm::length(glm::vec2(player.position.x-m_position.x,
                                                 player.position.z-m_position.z));
    const glm::vec3 eye = m_position+glm::vec3(0,1.6f,0);
    const glm::vec3 target = player.position+glm::vec3(0,1.0f,0);
    face_towards(player.position);
    switch (m_phase) {
        case 0: // disappearance -> appear on one of three sides, not in blocks
            m_mob.set_roll_degrees(0.0f);
            if (m_timer >= 4.3f) {
                const glm::vec3 behind = unit_or(glm::vec3(player.forward.x,0,player.forward.z),glm::vec3(0,0,-1));
                const glm::vec3 flank(-behind.z,0,behind.x);
                const float side = (m_generic_combo_sequence%2==0) ? 1.0f : -1.0f;
                const glm::vec3 candidate = player.position - behind*2.4f + flank*(side*.6f);
                if (!chunks || !body_blocked(*chunks,candidate,1.10f,3.6f)) m_position=candidate;
                m_generic_combo = m_generic_combo_sequence++ % 3;
                m_generic_hit_mask=0;
                m_phase=1; m_timer=0;
            }
            break;
        case 1: // short, readable tell before the ambush
            m_mob.set_roll_pivot_height(1.4f);
            m_mob.set_roll_degrees(12.0f * std::min(1.0f,m_timer/.64f));
            if (m_timer>=.64f) { m_phase=2; m_timer=0; }
            break;
        case 2: { // hook -> second slice, or long sliding cut
            const bool can_hit = distance<=(m_generic_combo==2 ? 3.25f : 2.65f) &&
                (!chunks || !blocked_segment(*chunks,eye,target));
            if (m_generic_combo==2 && m_timer<.35f) {
                const glm::vec3 candidate=m_position+towards*(4.0f*dt);
                if (!chunks || !body_blocked(*chunks,candidate,1.10f,3.6f)) m_position=candidate;
            }
            const float first = m_generic_combo==0 ? .22f : (m_generic_combo==1 ? .17f : .32f);
            if (!(m_generic_hit_mask&1u) && m_timer>=first) {
                m_generic_hit_mask|=1u;
                if (can_hit) result.player_damage+=6;
            }
            if (m_generic_combo==1 && !(m_generic_hit_mask&2u) && m_timer>=.48f) {
                m_generic_hit_mask|=2u;
                if (can_hit) result.player_damage+=5;
            }
            m_mob.set_roll_degrees(14.0f*std::sin(m_timer*7.0f));
            if (m_timer>=.63f) { m_phase=3;m_timer=0; }
            break;
        }
        case 3:
            m_mob.set_roll_degrees(0.0f);
            if (m_timer>=.63f) { m_phase=0; m_timer=0; }
            break;
        default: m_phase=0;m_timer=0;break;
    }
    return result;
}

Boss_Tick_Result Boss::update_moon(float dt, const Boss_Player_View& player,
                                  Chunk_Manager* chunks, Mob_Nav::System* navigation) {
    Boss_Tick_Result result;
    const glm::vec3 player_eye = player.position + glm::vec3(0.0f, 1.5f, 0.0f);
    const glm::vec3 away = unit_or(glm::vec3(m_position.x - player.position.x,0,m_position.z - player.position.z),glm::vec3(1,0,0));
    // A protected boss must still be beatable without a ranged weapon.
    // Periodic low swoops bring orbiting shields and the core within melee reach.
    const bool swooping = Celestial_Boss_Rules::moon_swooping(m_orbit_time);
    const char* anim = m_attack_timer > 1.75f ? "Moon_Cast" :
                       swooping ? "Moon_Swoop" : "Moon_Idle";
    if (m_mob.has_animation(anim)) m_mob.play_animation(anim, true);
    // The cast windup lasts ~.95s, while its authored clip spans 2.7s.
    m_mob.set_animation_speed(m_attack_timer > 1.75f ? 2.7f/.95f : 1.0f);
    const float radius = swooping ? 2.55f : 6.4f;
    const float altitude = swooping ? 0.45f : 2.5f + 0.4f*std::sin(m_orbit_time);
    const glm::vec3 target = player.position + away * radius + glm::vec3(0,altitude,0);
    if (glm::distance(player.position,m_position) < 42.0f) {
        fly_towards(dt, target, 3.1f, chunks, navigation);
        face_towards(player.position);
        m_attack_timer += dt;
        // Telegraph before a one/two/three-blade sequence. The princess'
        // unique mechanic is still her shield gate, not player adaptation.
        m_mob.set_roll_pivot_height(1.65f);
        m_mob.set_roll_degrees(m_attack_timer > 1.8f ? 10.0f * std::sin((m_attack_timer-1.8f)*2.4f) : 0.0f);
        if (m_attack_timer >= 2.7f) {
            m_attack_timer = 0.0f;
            const unsigned pattern = m_moon_attack_sequence++ % 3;
            const int blade_count = pattern == 0 ? 1 : (pattern == 1 ? 2 : 3);
            const glm::vec3 origin = m_position + glm::vec3(0,1.8f,0);
            const glm::vec3 forward = unit_or(player_eye-origin,glm::vec3(0,0,-1));
            const glm::vec3 horizontal_right = unit_or(glm::vec3(forward.z,0,-forward.x),glm::vec3(1,0,0));
            for (int i = 0; i < blade_count && m_blades.size() < 12; ++i) {
                Wind_Blade blade;
                blade.position = origin;
                const float spread = (static_cast<float>(i) - .5f*(blade_count-1)) * .26f;
                blade.direction = unit_or(forward + horizontal_right*spread,forward);
                if (blade.visual.load("assets/mobs/moon_wind_blade.glb")) {
                    blade.visual.set_scale(0.85f);
                    blade.visual.set_position(blade.position);
                    blade.visual.set_yaw_degrees(glm::degrees(std::atan2(blade.direction.x,blade.direction.z)));
                    m_blades.push_back(std::move(blade));
                }
            }
        }
    }
    for (auto& blade : m_blades) {
        const glm::vec3 next = blade.position + blade.direction * (kBladeSpeed * dt);
        blade.lifetime += dt;
        if (chunks && blocked_segment(*chunks,blade.position,next)) { blade.lifetime = kBladeLifetime; continue; }
        const glm::vec3 delta = next - blade.position;
        const float len = glm::length(delta);
        const glm::vec3 player_min = player.position + glm::vec3(-0.42f,0.0f,-0.42f);
        const glm::vec3 player_max = player.position + glm::vec3( 0.42f,1.9f, 0.42f);
        if (player.alive && mob_ray_aabb(blade.position, blade.direction, player_min, player_max, len) >= 0.0f) {
            result.player_damage += 7;
            blade.lifetime = kBladeLifetime;
        } else {
            blade.position = next;
            blade.visual.set_position(next);
        }
    }
    m_blades.erase(std::remove_if(m_blades.begin(),m_blades.end(),[](const Wind_Blade& blade) {
        return blade.lifetime >= kBladeLifetime;
    }),m_blades.end());
    return result;
}

Boss_Tick_Result Boss::update_sun(float dt, const Boss_Player_View& player,
                                 Chunk_Manager* chunks, Mob_Nav::System* navigation) {
    using namespace Boss_Choreography;
    Boss_Tick_Result result;
    const float initial_distance = glm::distance(player.position, m_position);
    if (initial_distance > 42.0f) {
        if (m_mob.has_animation("Sun_Idle")) m_mob.play_animation("Sun_Idle",true);
        m_sun_move = Sun_Move::Idle;
        m_sun_timeline = 0.0f;
        m_sun_active_hand = -1;
        m_mob.set_roll_degrees(0.0f);
        return result;
    }
    const int hands = active_parts();
    const auto move_animation = [](Sun_Move move) -> const char* {
        switch (move) {
            case Sun_Move::Palm_Lance: return "Sun_Palm_Lance";
            case Sun_Move::Twin_Seal: return "Sun_Twin_Seal";
            case Sun_Move::Halo_Sweep: return "Sun_Halo_Sweep";
            case Sun_Move::Sun_Dash: return "Sun_Sun_Dash";
            case Sun_Move::Air_Step: return "Sun_Air_Step";
            case Sun_Move::Rising_Kick: return "Sun_Rising_Kick";
            case Sun_Move::Crown_Dance: return "Sun_Crown_Dance";
            case Sun_Move::Falling_Star: return "Sun_Falling_Star";
            case Sun_Move::Delayed_Palm: return "Sun_Delayed_Palm";
            default: return "Sun_Idle";
        }
    };
    if (m_sun_move == Sun_Move::Idle) {
        m_sun_move = choose_sun_move(hands, initial_distance, m_sun_previous, m_sun_sequence++);
        m_sun_timeline = 0.0f;
        m_sun_active_hand = -1;
        const char* anim = move_animation(m_sun_move);
        if (m_mob.has_animation(anim)) m_mob.play_animation(anim, false);
    }
    // A broken hand is never required to damage the core. All four may be
    // alive when the princess dies; no health-gated phase transition exists.
    if (uses_hands(m_sun_move) && hands == 0) {
        m_sun_move = Sun_Move::Air_Step;
        m_sun_timeline = 0.0f;
        if (m_mob.has_animation("Sun_Air_Step")) m_mob.play_animation("Sun_Air_Step", false);
    }
    const Move_Spec spec = sun_spec(m_sun_move);
    const float old_t = m_sun_timeline;
    m_sun_timeline += dt;
    const float t = m_sun_timeline;
    const bool active = t >= spec.tell && t <= spec.tell + spec.active;
    const bool recovering = t > spec.tell + spec.active;

    // The air is a virtual floor: standing, circling, side-stepping and
    // launching into attacks do not require a voxel under the feet.
    const glm::vec3 away = unit_or(glm::vec3(m_position.x-player.position.x,0,
                                           m_position.z-player.position.z),glm::vec3(1,0,0));
    const glm::vec3 tangent(-away.z,0,away.x);
    const float side = (m_sun_sequence & 1u) ? 1.0f : -1.0f;
    float orbit_radius = hands == 0 ? 2.25f : 3.65f;
    float altitude = hands == 0 ? .45f : 1.15f;
    float lateral = side * (hands == 0 ? 1.2f : 1.8f);
    float speed = Celestial_Boss_Rules::sun_speed(m_part_health);
    if (m_sun_move == Sun_Move::Sun_Dash) {
        orbit_radius = active ? 1.25f : (recovering ? 3.0f : 5.4f);
        speed += active ? 8.0f : 1.5f;
        lateral *= .3f;
    } else if (m_sun_move == Sun_Move::Air_Step) {
        lateral *= active ? 2.8f : 1.6f;
        speed += active ? 5.5f : 1.0f;
    } else if (m_sun_move == Sun_Move::Rising_Kick) {
        orbit_radius = active ? 1.5f : 3.0f;
        altitude += active ? 1.4f : -.3f;
        speed += active ? 5.0f : 0.0f;
    } else if (m_sun_move == Sun_Move::Falling_Star) {
        orbit_radius = 1.55f;
        altitude = active ? .28f : 3.7f;
        speed += active ? 8.5f : 0.0f;
    } else if (m_sun_move == Sun_Move::Crown_Dance || m_sun_move == Sun_Move::Halo_Sweep) {
        lateral *= active ? 2.7f : 1.2f;
        orbit_radius = active ? 2.0f : orbit_radius;
        speed += active ? 4.0f : 0.0f;
    } else if (m_sun_move == Sun_Move::Delayed_Palm) {
        orbit_radius = active ? 1.55f : 2.7f;
        speed += active ? 5.0f : 0.0f;
    }
    if (recovering) speed *= .55f;
    const float hover = .08f * std::sin(m_orbit_time*3.0f);
    const glm::vec3 target = player.position + away*orbit_radius + tangent*lateral +
                             glm::vec3(0,altitude+hover,0);
    // Author-directed close-quarters steps are immediate, rather than waiting
    // for a new async A* request each time the choreographed target changes.
    // The navigation system resumes for approach, anticipation and recovery.
    const bool quick_step = active && (m_sun_move == Sun_Move::Air_Step ||
        m_sun_move == Sun_Move::Sun_Dash || m_sun_move == Sun_Move::Crown_Dance ||
        m_sun_move == Sun_Move::Falling_Star || m_sun_move == Sun_Move::Rising_Kick);
    if (quick_step) {
        m_flight_agent.reset();
        const glm::vec3 desired_velocity = unit_or(target - m_position,glm::vec3(0)) * speed;
        for (int substep=0; substep<3; ++substep) {
            m_mob.move_flying(dt/3.0f,desired_velocity,[&](const glm::vec3& feet) {
                return chunks && body_blocked(*chunks,feet);
            });
        }
        m_position = m_mob.get_position();
    } else {
        fly_towards(dt,target,speed,chunks,navigation);
    }
    face_towards(player.position);
    m_sun_focus = player.position;
    m_sun_active_hand = -1;
    if (uses_hands(m_sun_move) && hands > 0) {
        float strongest_pulse = 0.0f;
        int strike_slot = -1;
        for (int i=0;i<3;++i) if (spec.strikes[i] >= 0.0f) {
            const float pulse = strike_pulse(t,spec.tell + spec.strikes[i]);
            if (pulse > strongest_pulse) { strongest_pulse=pulse; strike_slot=i; }
        }
        if (strike_slot >= 0) {
            int ordinal = 0;
            for (int hand=0; hand<4; ++hand) if (m_part_health[hand] > 0.0f) {
                if (ordinal++ == (strike_slot%hands)) { m_sun_active_hand=hand; break; }
            }
        }
    }

    // Procedural torso accents are visible even on the original unrigged GLB.
    // This does not pretend to provide articulated arm/leg keyframes.
    m_mob.set_roll_pivot_height(1.6f);
    float tilt = active ? 14.0f : (recovering ? -6.0f : 5.0f);
    if (m_sun_move == Sun_Move::Crown_Dance || m_sun_move == Sun_Move::Halo_Sweep)
        tilt += active ? 12.0f*std::sin((t-spec.tell)*9.0f) : 0.0f;
    if (m_sun_move == Sun_Move::Falling_Star) tilt = active ? 22.0f : -12.0f;
    m_mob.set_roll_degrees(side * tilt);

    for (int i = 0; i < 3; ++i) {
        if (spec.strikes[i] < 0.0f) continue;
        if (!in_strike_window(spec,i,old_t,t)) continue;
        // Impact is strictly time-gated and cannot repeat from adjacent ticks.
        const glm::vec3 source = m_position + glm::vec3(0,1.55f,0);
        const glm::vec3 victim = player.position + glm::vec3(0,1.0f,0);
        const bool reachable = glm::distance(source,victim) <= spec.range;
        if (reachable && (!chunks || !blocked_segment(*chunks,source,victim)))
            result.player_damage += spec.damage;
    }
    if (t >= spec.tell + spec.active + spec.recovery) {
        m_sun_previous = m_sun_move;
        m_sun_move = Sun_Move::Idle;
        m_sun_timeline = 0.0f;
        m_sun_active_hand = -1;
        if (m_mob.has_animation("Sun_Idle")) m_mob.play_animation("Sun_Idle", true);
    }
    return result;
}

Mob_Hit_Result Boss::raycast_body_part(const glm::vec3& eye, const glm::vec3& direction, float reach) const {
    Mob_Hit_Result hit = m_mob.raycast_body_part(eye,direction,reach);
    for (int i = 0; i < 4; ++i) {
        if (m_part_health[i] <= 0.0f) continue;
        const glm::vec3 p = satellite_position(i);
        const glm::vec3 radius = m_kind == Boss_Kind::Moon_Princess ? glm::vec3(.44f,.52f,.30f) : glm::vec3(.59f,.74f,.36f);
        const float dist = mob_ray_aabb(eye,direction,p-radius,p+radius,reach);
        if (dist >= 0.0f && (!hit || dist < hit.distance)) {
            hit.distance = dist;
            hit.zone = Mob_Hit_Zone::Torso;
            hit.subpart = i;
        }
    }
    return hit;
}

bool Boss::take_damage(float amount, const glm::vec3& attacker_position, int satellite) {
    (void)attacker_position;
    if (m_dying || amount <= 0.0f) return false;
    if ((m_kind == Boss_Kind::Moon_Princess || m_kind == Boss_Kind::Sun_Princess) && satellite >= 0) {
        if (Celestial_Boss_Rules::damage_satellite(m_part_health,satellite,amount)) {
            m_part_flash[satellite] = kHurtFlashSeconds;
        }
        return false;
    }
    if (m_kind == Boss_Kind::Moon_Princess && !Celestial_Boss_Rules::moon_vulnerable(m_part_health)) return false;
    m_health = std::max(0.0f, m_health - amount);
    m_hurt_timer = kHurtFlashSeconds;
    return m_health <= 0.0f && kill();
}

bool Boss::kill() {
    if (m_dying) return false;
    m_health = 0.0f;
    m_dying = true;
    m_death_timer = 0.0f;
    m_blades.clear();
    m_sun_move = Boss_Choreography::Sun_Move::Idle;
    m_sun_active_hand = -1;
    m_mob.set_roll_pivot_height(boss_stats(m_kind).scale * 0.5f);
    m_mob.set_roll_degrees(90.0f);
    return true;
}

void Boss::compute_hitbox(glm::vec3& out_min, glm::vec3& out_max) const {
    const float s = boss_stats(m_kind).scale;
    const float half = 0.6f * s;
    const float height = (m_kind == Boss_Kind::Sun_Princess || m_kind == Boss_Kind::Moon_Princess) ? 3.25f : 2.0f*s;
    out_min = glm::vec3(m_position.x-half,m_position.y,m_position.z-half);
    out_max = glm::vec3(m_position.x+half,m_position.y+height,m_position.z+half);
}

void Boss::render(const Shader& shader, const glm::mat4& view, const glm::mat4& projection, float ambient) const {
    m_mob.render(shader,view,projection,ambient);
    if (m_dying) return;
    if (m_kind == Boss_Kind::Moon_Princess || m_kind == Boss_Kind::Sun_Princess) {
        for (int i = 0; i < 4; ++i) if (m_part_health[i] > 0.0f)
            m_satellites[i].render(shader,view,projection,ambient);
    }
    for (const auto& blade : m_blades) blade.visual.render(shader,view,projection,ambient);
}
