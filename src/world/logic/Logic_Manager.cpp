#include "utils/configs/Config_World.h"
#include "utils/profiling/Profiler.h"
#include "world/logic/Logic_Manager.h"
#include "world/chunks/Chunk_Manager.h"
#include "world/blocks/Block_Entity.h"
#include <algorithm>

namespace {

// +X, -X, +Y, -Y, +Z, -Z — как грани в Block_Face.
const glm::ivec3 kDirs[6] = {
    {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}
};

Redstone_Key key_of(glm::ivec3 p) { return {p.x, p.y, p.z}; }
glm::ivec3 pos_of(const Redstone_Key& k) { return {k[0], k[1], k[2]}; }

// Смещение выхода повторителя по направлению (Block_Meta::facing_*).
glm::ivec3 facing_offset(uint8_t meta) {
    switch (meta & Block_Meta::direction_mask) {
        case Block_Meta::facing_north: return {0, 0, -1};
        case Block_Meta::facing_east:  return {1, 0, 0};
        case Block_Meta::facing_south: return {0, 0, 1};
        default:                       return {-1, 0, 0};
    }
}

// Задержка повторителя в редстоун-тиках: биты 2..3 хранят 0..3 → 1..4.
int repeater_delay(uint8_t meta) { return ((meta >> 2) & 3) + 1; }

// Same-plane wires, plus one explicit extension at the base of a wall wire.
// Matches wire_shape_meta() and the GPU wire mesh (including the 90-degree riser).
bool wire_linked(glm::ivec3 a,uint8_t am,glm::ivec3 b,uint8_t bm) {
    am &= 7u; bm &= 7u;
    if (am==bm) {
        const glm::ivec3 d=b-a;
        if (am==0u) return d.y==0;
        if (am==1u || am==2u) return d.x==0;
        if (am==3u || am==4u) return d.z==0;
        return false;
    }
    const glm::ivec3 wall=am==0u?b:a;
    const glm::ivec3 floor=am==0u?a:b;
    const uint8_t mounting=am==0u?bm:am;
    const glm::ivec3 expected=mounting==1u?glm::ivec3(1,0,0):
        mounting==2u?glm::ivec3(-1,0,0):
        mounting==3u?glm::ivec3(0,0,1):
        mounting==4u?glm::ivec3(0,0,-1):glm::ivec3(0,0,0);
    return mounting>=1u && mounting<=4u && wall-floor==expected;
}

constexpr uint8_t kAttachMask = 0x07;     // грань крепления (индекс в kDirs)
constexpr uint8_t kDelayMask = 0x0C;      // биты задержки повторителя

// Сигнал, который узел n выдаёт в соседнюю клетку c.
int emit(Block_Types type, uint8_t meta, glm::ivec3 n, glm::ivec3 c) {
    switch (type) {
        case Block_Types::Lever_On:
        case Block_Types::Button_Pressed:
            return 15;
        case Block_Types::Signal_Clamp:
            return c == Logic_Manager::support_of(n, type, meta) ? 0 : 15;
        case Block_Types::Repeater_On:
            return c == n + facing_offset(meta) ? 15 : 0;
        case Block_Types::Observer:
            return (meta & Block_Meta::active_bit) != 0u && c == n - facing_offset(meta) ? 15 : 0;
        default:
            // Wooden/stone/metal button variants use their original id and the active_bit
            // in metadata instead of allocating an extra pressed block type for each wood.
            return get_block_props(type).shape == Block_Shape::Button &&
                   (meta & Block_Meta::active_bit) != 0u ? 15 : 0;
    }
}

// Чтение мира для одного шага симуляции. Непрогруженные клетки — воздух.
struct Sim {
    const Logic_Manager::World_Access& world;
    std::map<Redstone_Key, int> wire_power;

    Block_Types type(glm::ivec3 p) const {
        return world.loaded(p.x, p.y, p.z) ? world.type(p.x, p.y, p.z) : Block_Types::Air;
    }
    uint8_t meta(glm::ivec3 p) const { return world.meta(p.x, p.y, p.z); }

    // Сигнал, который твёрдый блок s получает от соседних узлов (без проводов — чтобы не было
    // циклов при расчёте проводов).
    int source_power(glm::ivec3 s) const {
        int best = 0;
        for (const glm::ivec3& d : kDirs) {
            const glm::ivec3 n = s + d;
            const Block_Types t = type(n);
            if (Logic_Manager::is_node(t) && !Logic_Manager::is_wire(t)) {
                best = std::max(best, emit(t, meta(n), n, s));
            }
        }
        return best;
    }

    // Весь сигнал, который приходит в клетку c от всех соседей (провода, узлы, твёрдые блоки).
    int full_power(glm::ivec3 c) const {
        int best = 0;
        for (const glm::ivec3& d : kDirs) {
            const glm::ivec3 n = c + d;
            const Block_Types t = type(n);
            if (Logic_Manager::is_wire(t)) {
                const auto it = wire_power.find(key_of(n));
                if (it != wire_power.end()) best = std::max(best, it->second);
            } else if (Logic_Manager::is_node(t)) {
                best = std::max(best, emit(t, meta(n), n, c));
            } else if (Logic_Manager::is_support(t)) {
                best = std::max(best, source_power(n));
            }
        }
        return best;
    }

    // Сигнал, который приходит в повторитель p со стороны входа back.
    int repeater_input(glm::ivec3 back, glm::ivec3 p) const {
        const Block_Types t = type(back);
        if (Logic_Manager::is_wire(t)) {
            const auto it = wire_power.find(key_of(back));
            return it == wire_power.end() ? 0 : it->second;
        }
        if (Logic_Manager::is_node(t)) return emit(t, meta(back), back, p);
        if (Logic_Manager::is_support(t)) return full_power(back);
        return 0;
    }
};

}  // namespace

// --- Классификация ----------------------------------------------------------------------------

bool Logic_Manager::is_wire(Block_Types type) {
    return type == Block_Types::Copper_Wire;
}

bool Logic_Manager::is_node(Block_Types type) {
    switch (type) {
        case Block_Types::Copper_Wire:
        case Block_Types::Lever:
        case Block_Types::Lever_On:
        case Block_Types::Button:
        case Block_Types::Button_Pressed:
        case Block_Types::Signal_Clamp:
        case Block_Types::Signal_Clamp_Off:
        case Block_Types::Repeater:
        case Block_Types::Repeater_On:
        case Block_Types::Observer:
        case Block_Types::Piston:
        case Block_Types::Sticky_Piston:
        case Block_Types::Copper_Lamp:
        case Block_Types::Copper_Lamp_On:
            return true;
        default:
            return get_block_props(type).shape == Block_Shape::Button;
    }
}

bool Logic_Manager::is_interactable(Block_Types t) {
    switch (t) {
        case Block_Types::Lever: case Block_Types::Lever_On:
        case Block_Types::Button: case Block_Types::Button_Pressed:
        case Block_Types::Repeater: case Block_Types::Repeater_On:
            return true;
        default: return get_block_props(t).shape == Block_Shape::Button;
    }
}

bool Logic_Manager::is_attached(Block_Types type) {
    switch (type) {
        case Block_Types::Lever:
        case Block_Types::Lever_On:
        case Block_Types::Button:
        case Block_Types::Button_Pressed:
        case Block_Types::Signal_Clamp:
        case Block_Types::Signal_Clamp_Off:
            return true;
        default:
            return get_block_props(type).shape == Block_Shape::Button;
    }
}

bool Logic_Manager::is_support(Block_Types type) {
    if (type == Block_Types::Air) return false;
    // Mechanism casings are solid supports, unlike wire / small source components.
    if (type == Block_Types::Piston || type == Block_Types::Sticky_Piston ||
        type == Block_Types::Copper_Lamp || type == Block_Types::Copper_Lamp_On ||
        type == Block_Types::Observer) return true;
    return !is_node(type) && get_block_props(type).is_solid;
}

Block_Types Logic_Manager::item_of(Block_Types type) {
    switch (type) {
        case Block_Types::Lever_On:         return Block_Types::Lever;
        case Block_Types::Button_Pressed:   return Block_Types::Button;
        case Block_Types::Signal_Clamp_Off: return Block_Types::Signal_Clamp;
        case Block_Types::Repeater_On:      return Block_Types::Repeater;
        default:                            return type;
    }
}

glm::ivec3 Logic_Manager::support_of(glm::ivec3 pos, Block_Types type, uint8_t meta) {
    if (is_wire(type)) {
        switch (meta & 7u) {
            case 1: return pos + glm::ivec3(1,0,0);
            case 2: return pos + glm::ivec3(-1,0,0);
            case 3: return pos + glm::ivec3(0,0,1);
            case 4: return pos + glm::ivec3(0,0,-1);
            default: return pos + glm::ivec3(0,-1,0); // old worlds: meta=0 = floor
        }
    }
    if (is_attached(type)) return pos + kDirs[meta & kAttachMask];
    return pos + glm::ivec3(0, -1, 0);
}

// --- Установка --------------------------------------------------------------------------------

bool Logic_Manager::placement(Block_Types type, const World_Access& world, glm::ivec3 place,
                              glm::ivec3 clicked, uint8_t repeater_facing, uint8_t& out_meta) {
    if (type == Block_Types::Observer || type == Block_Types::Piston ||
        type == Block_Types::Sticky_Piston || type == Block_Types::Copper_Lamp) {
        out_meta = (type == Block_Types::Copper_Lamp) ? 0u :
            static_cast<uint8_t>(repeater_facing & Block_Meta::direction_mask);
        return true;
    }
    if (is_wire(type)) {
        const glm::ivec3 d=clicked-place;
        uint8_t mounting=0;
        if (d == glm::ivec3(0,-1,0)) mounting=0; // floor
        else if (d == glm::ivec3(1,0,0)) mounting=1;
        else if (d == glm::ivec3(-1,0,0)) mounting=2;
        else if (d == glm::ivec3(0,0,1)) mounting=3;
        else if (d == glm::ivec3(0,0,-1)) mounting=4;
        else return false;
        if (!is_support(world.type(clicked.x,clicked.y,clicked.z))) return false;
        out_meta=mounting;
        return true;
    }
    if (type == Block_Types::Repeater) {
        if (!is_support(world.type(place.x, place.y - 1, place.z))) return false;
        out_meta = static_cast<uint8_t>(repeater_facing & Block_Meta::direction_mask);
        return true;
    }
    if (is_attached(type)) {
        const glm::ivec3 diff = clicked - place;
        for (uint8_t i = 0; i < 6; ++i) {
            if (kDirs[i] == diff) {
                if (!is_support(world.type(clicked.x, clicked.y, clicked.z))) return false;
                out_meta = i;
                return true;
            }
        }
    }
    return false;
}

bool Logic_Manager::has_support_for(const World_Access& world, glm::ivec3 pos, Block_Types type, uint8_t meta) {
    // Only mounted components need a supporting neighbor. Full-cube mechanisms such
    // as pistons, observers and lamps must not break when the block below is removed.
    if (!is_wire(type) && !is_attached(type) &&
        type != Block_Types::Repeater && type != Block_Types::Repeater_On) return true;
    const glm::ivec3 s = support_of(pos, type, meta);
    return is_support(world.type(s.x, s.y, s.z));
}

std::vector<glm::ivec3> Logic_Manager::nodes_without_support(const World_Access& world, glm::ivec3 pos) {
    std::vector<glm::ivec3> lost;
    for (const glm::ivec3& d : kDirs) {
        const glm::ivec3 n = pos + d;
        const Block_Types t = world.type(n.x, n.y, n.z);
        if (!is_node(t)) continue;
        if (!has_support_for(world, n, t, world.meta(n.x, n.y, n.z))) lost.push_back(n);
    }
    return lost;
}

// --- Действия игрока --------------------------------------------------------------------------

bool Logic_Manager::interact(const World_Access& world, glm::ivec3 pos) {
    const Block_Types type = world.type(pos.x, pos.y, pos.z);
    const uint8_t meta = world.meta(pos.x, pos.y, pos.z);
    switch (type) {
        case Block_Types::Lever:
            write_block(world, pos, Block_Types::Lever_On, meta);
            track(pos);
            return true;
        case Block_Types::Lever_On:
            write_block(world, pos, Block_Types::Lever, meta);
            track(pos);
            return true;
        case Block_Types::Button:
            write_block(world, pos, Block_Types::Button_Pressed, meta);
            m_pulses[key_of(pos)] = k_button_ticks;
            track(pos);
            return true;
        case Block_Types::Button_Pressed:
            return true;   // уже нажата — ничего не меняем
        case Block_Types::Repeater:
        case Block_Types::Repeater_On: {
            const int next = repeater_delay(meta) % 4 + 1;   // 1 → 2 → 3 → 4 → 1
            const uint8_t new_meta = static_cast<uint8_t>((meta & ~kDelayMask) | (((next - 1) & 3) << 2));
            write_block(world, pos, type, new_meta);
            track(pos);
            return true;
        }
        default:
            if (get_block_props(type).shape == Block_Shape::Button) {
                if ((meta & Block_Meta::active_bit) != 0u) return true;
                write_block(world, pos, type, static_cast<uint8_t>(meta | Block_Meta::active_bit));
                m_pulses[key_of(pos)] = k_button_ticks;
                track(pos);
                return true;
            }
            return false;
    }
}

// --- Симуляция --------------------------------------------------------------------------------

void Logic_Manager::track(glm::ivec3 pos) {
    m_nodes.insert(key_of(pos));
}

void Logic_Manager::discover(const World_Access& world, glm::ivec3 center, int radius) {
    for (int x = center.x - radius; x <= center.x + radius; ++x) {
        for (int y = center.y - radius; y <= center.y + radius; ++y) {
            for (int z = center.z - radius; z <= center.z + radius; ++z) {
                if (!world.loaded(x, y, z)) continue;
                const Block_Types t = world.type(x, y, z);
                if (!is_node(t)) continue;
                const Redstone_Key k{x, y, z};
                m_nodes.insert(k);
                // Кнопка, сохранённая нажатой: таймер заново, иначе она залипнет навсегда.
                if ((t == Block_Types::Button_Pressed ||
                     (get_block_props(t).shape == Block_Shape::Button &&
                      (world.meta(x,y,z) & Block_Meta::active_bit) != 0u)) &&
                    m_pulses.find(k) == m_pulses.end()) {
                    m_pulses[k] = k_button_ticks;
                }
            }
        }
    }
}

void Logic_Manager::update(const World_Access& world, float dt) {
    OC_ZONE("Logic::update");
    m_accumulator += dt;
    int guard = 0;   // не больше 10 тиков за кадр, чтобы не уйти в спираль после паузы
    while (m_accumulator >= k_redstone_tick_seconds && guard++ < 10) {
        step(world);
        m_accumulator -= k_redstone_tick_seconds;
    }
    if (guard >= 10) m_accumulator = 0.0f;
}

// Piston changes are performed in the existing logic tick, no worker queue.
// Conservative atomic preflight: all destination cells loaded, no block entities or
// unbreakable/foreign piston heads, at most 12 movable blocks. We do not move entities.
bool Logic_Manager::piston_set(const World_Access& world, glm::ivec3 pos,
                               Block_Types type, uint8_t meta, bool extend) {
    const glm::ivec3 dir=facing_offset(meta);
    const glm::ivec3 front=pos+dir;
    if (extend) {
        if (!world.loaded(front.x,front.y,front.z)) return false;
        int count=0;
        glm::ivec3 scan=front;
        while (world.type(scan.x,scan.y,scan.z)!=Block_Types::Air) {
            if (++count>12 || !world.loaded(scan.x,scan.y,scan.z)) return false;
            const Block_Types occupied=world.type(scan.x,scan.y,scan.z);
            const auto& props=get_block_props(occupied);
            if (props.unbreakable() || occupied==Block_Types::Piston_Head ||
                occupied==Block_Types::Piston || occupied==Block_Types::Sticky_Piston ||
                props.state==Block_State::Liquid || is_node(occupied) ||
                props.shape==Block_Shape::Door || props.shape==Block_Shape::Bed ||
                block_entity_type_for(occupied)!=Block_Entity_Type::None) return false;
            scan+=dir;
            if (!world.loaded(scan.x,scan.y,scan.z)) return false;
        }
        // From farthest to nearest: blocks never overwrite unread sources.
        for (int i=count-1;i>=0;--i) {
            const glm::ivec3 src=front+dir*i;
            const glm::ivec3 dst=src+dir;
            write_block(world,dst,world.type(src.x,src.y,src.z),world.meta(src.x,src.y,src.z));
        }
        write_block(world,front,Block_Types::Piston_Head,static_cast<uint8_t>(meta & 3u));
        write_block(world,pos,type,static_cast<uint8_t>(meta|Block_Meta::active_bit));
        return true;
    }
    if (!world.loaded(front.x,front.y,front.z)) return false;
    if (world.type(front.x,front.y,front.z)==Block_Types::Piston_Head) {
        write_block(world,front,Block_Types::Air,0u);
    }
    if (type==Block_Types::Sticky_Piston) {
        const glm::ivec3 src=front+dir;
        if (world.loaded(src.x,src.y,src.z)) {
            const Block_Types movable=world.type(src.x,src.y,src.z);
            const auto& props=get_block_props(movable);
            if (movable!=Block_Types::Air && movable!=Block_Types::Piston_Head &&
                movable!=Block_Types::Piston && movable!=Block_Types::Sticky_Piston &&
                !props.unbreakable() && props.state!=Block_State::Liquid && !is_node(movable) &&
                props.shape!=Block_Shape::Door && props.shape!=Block_Shape::Bed &&
                block_entity_type_for(movable)==Block_Entity_Type::None &&
                world.type(front.x,front.y,front.z)==Block_Types::Air) {
                write_block(world,front,movable,world.meta(src.x,src.y,src.z));
                write_block(world,src,Block_Types::Air,0u);
            }
        }
    }
    write_block(world,pos,type,static_cast<uint8_t>(meta & ~Block_Meta::active_bit));
    return true;
}

void Logic_Manager::step(const World_Access& world) {
    Sim sim{world, {}};

    // 1. Забыть узлы, которых больше нет (сломаны), и их таймеры. Непрогруженные — не трогаем.
    for (auto it = m_nodes.begin(); it != m_nodes.end();) {
        const glm::ivec3 p = pos_of(*it);
        if (!world.loaded(p.x, p.y, p.z)) { ++it; continue; }
        if (!is_node(world.type(p.x, p.y, p.z))) {
            m_pending.erase(*it);
            m_pulses.erase(*it);
            m_observed_state.erase(*it);
            m_observer_pulse.erase(*it);
            it = m_nodes.erase(it);
        } else {
            ++it;
        }
    }

    // 2. Сигнал проводов по состоянию на начало тика: от источников, затем на −1 по проводам.
    // Signal attenuation is bounded to 15. Propagate high-to-low with 16 buckets
    // rather than repeatedly sweeping every wire until no changes remain.
    std::array<std::vector<glm::ivec3>,16> power_buckets;
    for (const Redstone_Key& k : m_nodes) {
        const glm::ivec3 p = pos_of(k);
        if (!world.loaded(p.x, p.y, p.z) || !is_wire(world.type(p.x, p.y, p.z))) continue;
        int seed = 0;
        for (const glm::ivec3& d : kDirs) {
            const glm::ivec3 n = p + d;
            const Block_Types t = sim.type(n);
            if (is_node(t) && !is_wire(t)) {
                seed = std::max(seed, emit(t, sim.meta(n), n, p));
            } else if (is_support(t)) {
                seed = std::max(seed, sim.source_power(n));
            }
        }
        sim.wire_power[k] = seed;
        if (seed > 1) power_buckets[seed].push_back(p);
    }
    for (int level=15;level>1;--level) {
        for (const glm::ivec3& p : power_buckets[level]) {
            const auto source_it=sim.wire_power.find(key_of(p));
            if (source_it==sim.wire_power.end() || source_it->second!=level) continue;
            for (const glm::ivec3& d : kDirs) {
                const glm::ivec3 n=p+d;
                auto it=sim.wire_power.find(key_of(n));
                if (it==sim.wire_power.end() || it->second>=level-1) continue;
                if (!wire_linked(p,sim.meta(p),n,sim.meta(n))) continue;
                it->second=level-1;
                if (level>2) power_buckets[level-1].push_back(n);
            }
        }
    }

    // 2b. Динамит: клетка рядом с узлом, на которую приходит сигнал, — к взрыву (Game).
    for (const Redstone_Key& k : m_nodes) {
        const glm::ivec3 p = pos_of(k);
        if (!world.loaded(p.x, p.y, p.z)) continue;
        for (const glm::ivec3& d : kDirs) {
            const glm::ivec3 c = p + d;
            if (sim.type(c) == Block_Types::Dynamite && sim.full_power(c) > 0) m_triggered.push_back(c);
        }
    }

    // 3. Желаемые состояния зажимателей и повторителей — все по одному снимку мира.
    struct Apply {
        glm::ivec3 pos;
        Block_Types target;
    };
    std::vector<Apply> to_apply;
    for (const Redstone_Key& k : m_nodes) {
        const glm::ivec3 p = pos_of(k);
        if (!world.loaded(p.x, p.y, p.z)) continue;
        const Block_Types t = world.type(p.x, p.y, p.z);
        const uint8_t meta = world.meta(p.x, p.y, p.z);

        Block_Types desired = t;
        int delay = 0;
        if (t == Block_Types::Signal_Clamp || t == Block_Types::Signal_Clamp_Off) {
            const bool on = sim.full_power(support_of(p, t, meta)) == 0;
            desired = on ? Block_Types::Signal_Clamp : Block_Types::Signal_Clamp_Off;
            delay = k_clamp_delay_ticks;
        } else if (t == Block_Types::Repeater || t == Block_Types::Repeater_On) {
            const glm::ivec3 out = facing_offset(meta);
            const int input = sim.repeater_input(p - out, p);
            // Блокировка: с боку в повторитель выдаёт сигнал другой включённый повторитель.
            bool locked = false;
            for (const glm::ivec3& d : kDirs) {
                if (d == out || d == -out || d.y != 0) continue;
                const glm::ivec3 s = p + d;
                if (sim.type(s) == Block_Types::Repeater_On && s + facing_offset(sim.meta(s)) == p) {
                    locked = true;
                }
            }
            if (!locked) {
                desired = input > 0 ? Block_Types::Repeater_On : Block_Types::Repeater;
                delay = repeater_delay(meta);
            }
        } else {
            continue;   // провода, рычаги и кнопки не переключаются по таймеру
        }

        if (desired == t) {
            m_pending.erase(k);
            continue;
        }
        const auto it = m_pending.find(k);
        if (it == m_pending.end() || it->second.target != desired) {
            m_pending[k] = Pending{delay, desired};
        } else if (--it->second.remaining <= 0) {
            to_apply.push_back({p, desired});
            m_pending.erase(it);
        }
    }

    // 4. Кнопки: отпускаются через k_button_ticks.
    for (auto it = m_pulses.begin(); it != m_pulses.end();) {
        const glm::ivec3 p = pos_of(it->first);
        if (!world.loaded(p.x, p.y, p.z)) { ++it; continue; }
        const Block_Types t = world.type(p.x, p.y, p.z);
        const uint8_t meta = world.meta(p.x, p.y, p.z);
        const bool vanilla_button = t == Block_Types::Button_Pressed;
        const bool variant_button = get_block_props(t).shape == Block_Shape::Button &&
                                    (meta & Block_Meta::active_bit) != 0u;
        if (!vanilla_button && !variant_button) {
            it = m_pulses.erase(it);
            continue;
        }
        if (--it->second <= 0) {
            if (vanilla_button) to_apply.push_back({p, Block_Types::Button});
            else write_block(world,p,t,static_cast<uint8_t>(meta & ~Block_Meta::active_bit));
            it = m_pulses.erase(it);
        } else {
            ++it;
        }
    }

    // 5. Применить переключения.
    for (const Apply& a : to_apply) {
        write_block(world, a.pos, a.target, world.meta(a.pos.x, a.pos.y, a.pos.z));
    }

    // 6. Observers: sample the state of the cell on the sensing face. Pulse lasts
    // one redstone step (2 game ticks); on a new observer initialize, do not pulse.
    // Polling cannot see changes that occurred and reverted between steps (documented limitation).
    for (const Redstone_Key& k : m_nodes) {
        const glm::ivec3 p=pos_of(k);
        if (!world.loaded(p.x,p.y,p.z) || world.type(p.x,p.y,p.z)!=Block_Types::Observer) continue;
        const uint8_t m=world.meta(p.x,p.y,p.z);
        const glm::ivec3 look=p+facing_offset(m);
        if (!world.loaded(look.x,look.y,look.z)) continue;
        const uint32_t observed=(static_cast<uint32_t>(world.type(look.x,look.y,look.z))<<8u)
                               |world.meta(look.x,look.y,look.z);
        const auto [it,inserted]=m_observed_state.try_emplace(k,observed);
        const bool modified=!inserted && it->second!=observed;
        it->second=observed;
        if ((m & Block_Meta::active_bit)!=0u) {
            auto pulse=m_observer_pulse.find(k);
            if (pulse==m_observer_pulse.end() || --pulse->second<=0) {
                write_block(world,p,Block_Types::Observer,static_cast<uint8_t>(m&~Block_Meta::active_bit));
                m_observer_pulse.erase(k);
            }
        } else if (modified) {
            write_block(world,p,Block_Types::Observer,static_cast<uint8_t>(m|Block_Meta::active_bit));
            m_observer_pulse[k]=1;
        }
    }

    // 7. Mechanisms and powered doors consume the signal calculated this step.
    // No new scheduling pipeline: changes are performed using the existing World_Access.
    std::set<Redstone_Key> door_candidates=m_powered_doors;
    for (const Redstone_Key& k : m_nodes) {
        const glm::ivec3 p=pos_of(k);
        if (!world.loaded(p.x,p.y,p.z)) continue;
        const Block_Types t=world.type(p.x,p.y,p.z);
        const uint8_t m=world.meta(p.x,p.y,p.z);
        if (t==Block_Types::Piston || t==Block_Types::Sticky_Piston) {
            const bool active=(m&Block_Meta::active_bit)!=0u;
            const bool powered=sim.full_power(p)>0;
            if (active!=powered) piston_set(world,p,t,m,powered);
            else if (active) {
                const glm::ivec3 front=p+facing_offset(m);
                if (world.loaded(front.x,front.y,front.z) &&
                    world.type(front.x,front.y,front.z)!=Block_Types::Piston_Head) {
                    // Head was destroyed separately; a powered base can retry extension.
                    write_block(world,p,t,static_cast<uint8_t>(m&~Block_Meta::active_bit));
                }
            }
        } else if (t==Block_Types::Copper_Lamp || t==Block_Types::Copper_Lamp_On) {
            const Block_Types target=sim.full_power(p)>0 ? Block_Types::Copper_Lamp_On : Block_Types::Copper_Lamp;
            if (t!=target) write_block(world,p,target,m);
        }
        // Find doors adjacent to any tracked source/wire, both upper and lower halves.
        for (const glm::ivec3& d : kDirs) {
            const glm::ivec3 q=p+d;
            if (!world.loaded(q.x,q.y,q.z)) continue;
            const Block_Types door=world.type(q.x,q.y,q.z);
            if (get_block_props(door).shape!=Block_Shape::Door) continue;
            const int bottom=q.y-((world.meta(q.x,q.y,q.z)&8u)!=0u ? 1 : 0);
            door_candidates.insert({q.x,bottom,q.z});
        }
    }
    for (const Redstone_Key& key : door_candidates) {
        const glm::ivec3 bottom=pos_of(key);
        const glm::ivec3 upper=bottom+glm::ivec3(0,1,0);
        if (!world.loaded(bottom.x,bottom.y,bottom.z) || !world.loaded(upper.x,upper.y,upper.z)) continue;
        const Block_Types t=world.type(bottom.x,bottom.y,bottom.z);
        if (get_block_props(t).shape!=Block_Shape::Door || world.type(upper.x,upper.y,upper.z)!=t) {
            m_powered_doors.erase(key);
            continue;
        }
        const uint8_t lower_meta=world.meta(bottom.x,bottom.y,bottom.z);
        const uint8_t upper_meta=world.meta(upper.x,upper.y,upper.z);
        if ((lower_meta&8u)!=0u || (upper_meta&8u)==0u) continue;
        const bool powered=sim.full_power(bottom)>0 || sim.full_power(upper)>0;
        const bool was_powered=m_powered_doors.count(key)!=0u;
        if (powered) m_powered_doors.insert(key);
        else m_powered_doors.erase(key);
        // Leave manually opened unpowered doors unchanged. Power drop closes powered doors.
        if (!powered && !was_powered) continue;
        const uint8_t open_bit=powered ? 4u : 0u;
        if ((lower_meta&4u)!=open_bit)
            write_block(world,bottom,t,static_cast<uint8_t>((lower_meta&~4u)|open_bit));
        if ((upper_meta&4u)!=open_bit)
            write_block(world,upper,t,static_cast<uint8_t>((upper_meta&~4u)|open_bit));
    }
}

// --- Запись и доступ к миру -------------------------------------------------------------------

void Logic_Manager::write_block(const World_Access& world, glm::ivec3 pos, Block_Types type, uint8_t meta) {
    world.write(pos.x, pos.y, pos.z, type, meta);
    m_changed.push_back(pos);
}

std::vector<glm::ivec3> Logic_Manager::take_changed() {
    std::vector<glm::ivec3> out;
    out.swap(m_changed);
    return out;
}

std::vector<glm::ivec3> Logic_Manager::take_triggered() {
    std::vector<glm::ivec3> out;
    out.swap(m_triggered);
    return out;
}

namespace {
// Номер чанка по координате (с учётом отрицательных значений).
int chunk_index(int v) {
    const int s = Config::chunk_size;
    return v >= 0 ? v / s : -((-v + s - 1) / s);
}
}  // namespace

Logic_Manager::World_Access make_chunk_world(Chunk_Manager& chunks) {
    return Logic_Manager::World_Access{
        [&chunks](int x, int y, int z) { return chunks.get_block_world(x, y, z); },
        [&chunks](int x, int y, int z) { return chunks.get_block_meta_world(x, y, z); },
        [&chunks](int x, int y, int z, Block_Types t, uint8_t m) { chunks.set_block_world(x, y, z, t, m); },
        [&chunks](int x, int y, int z) {
            return chunks.is_chunk_loaded(chunk_index(x), chunk_index(y), chunk_index(z));
        }};
}
