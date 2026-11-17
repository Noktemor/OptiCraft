#include "utils/configs/Config_Player.h"
#include "utils/configs/Config_World.h"
#include <array>
#include <random>
#include <set>
#include "world/blocks/Block_Interaction.h"
#include "world/blocks/Block_Shapes.h"
#include <glad/gl.h>
#include "world/chunks/Chunk_Manager.h"
#include "world/blocks/Block_Entity.h"
#include "input/Input_Manager.h"
#include "rendering/Renderer.h"
#include "entities/player/Player.h"
#include "entities/player/Inventory.h"
#include "entities/animals/Pig.h"
#include "entities/items/Dropped_Item.h"
#include "math/Math_Helpers.h"
#include "world/crafting/Crop_Manager.h"
#include "world/crafting/Sapling_Manager.h"
#include "world/logic/Logic_Manager.h"

#include <algorithm>
#include <cmath>

namespace {
    constexpr int MOUSE_BUTTON_LEFT = 0;
    constexpr int MOUSE_BUTTON_RIGHT = 1;

    // Legacy notification used by block actions. Chunk_Manager::set_block_world(),
    // set_block_meta_world() and set_blocks_world_batch() already mark the affected
    // sections and neighbouring chunks dirty and queue the mesh rebuild. Deleting
    // Renderer GPU residency here destroyed BOTH active and pending COW generations,
    // causing an entire chunk section to disappear until the asynchronous upload.
    // Keep this helper as a compatibility shim for existing gameplay call sites;
    // actual GPU cache invalidation is reserved for chunk unload/world replacement.
    void invalidate_chunk_and_neighbors(int wx, int wy, int wz,
                                        Chunk_Manager& chunk_manager,
                                        Renderer& renderer) {
        (void)wx;
        (void)wy;
        (void)wz;
        (void)chunk_manager;
        (void)renderer;
    }

    // Пересечение луча с AABB (slab-метод). Возвращает расстояние вдоль луча или -1, если промах.
    float ray_vs_aabb(const glm::vec3& origin, const glm::vec3& dir,
                      const glm::vec3& box_min, const glm::vec3& box_max) {
        float t_near = 0.0f;
        float t_far = 1e9f;
        for (int axis = 0; axis < 3; ++axis) {
            if (std::abs(dir[axis]) < 1e-6f) {
                if (origin[axis] < box_min[axis] || origin[axis] > box_max[axis]) return -1.0f;
                continue;
            }
            float t1 = (box_min[axis] - origin[axis]) / dir[axis];
            float t2 = (box_max[axis] - origin[axis]) / dir[axis];
            if (t1 > t2) std::swap(t1, t2);
            t_near = std::max(t_near, t1);
            t_far = std::min(t_far, t2);
            if (t_near > t_far) return -1.0f;
        }
        return t_near;
    }
}

void invalidate_block_mesh(int wx, int wy, int wz, Chunk_Manager& chunk_manager, Renderer& renderer) {
    invalidate_chunk_and_neighbors(wx, wy, wz, chunk_manager, renderer);
}

namespace {
// A door occupies exactly two cells, sharing its block id, facing and open bit.
// Meta bit 3 marks the upper half. Do not create a second drop for that half.
void remove_door_companion(const glm::ivec3& at, Block_Types type, uint8_t meta,
                           Chunk_Manager& manager, Renderer& renderer) {
    if (get_block_props(type).shape != Block_Shape::Door) return;
    const int other_y = at.y + ((meta & 8u) != 0u ? -1 : 1);
    if (manager.get_block_world(at.x, other_y, at.z) != type) return;
    const uint8_t other_meta = manager.get_block_meta_world(at.x, other_y, at.z);
    if (((meta ^ other_meta) & 8u) == 0u) return;
    if (manager.set_block_world(at.x, other_y, at.z, Block_Types::Air))
        invalidate_chunk_and_neighbors(at.x, other_y, at.z, manager, renderer);
}
void remove_piston_companion(const glm::ivec3& at, Block_Types type, uint8_t meta,
                             Chunk_Manager& manager, Renderer& renderer) {
    static constexpr glm::ivec3 facing[4]={{0,0,-1},{1,0,0},{0,0,1},{-1,0,0}};
    if (type==Block_Types::Piston || type==Block_Types::Sticky_Piston) {
        if ((meta&Block_Meta::active_bit)==0u) return;
        const glm::ivec3 head=at+facing[meta&3u];
        if (manager.get_block_world(head.x,head.y,head.z)!=Block_Types::Piston_Head) return;
        if (manager.set_block_world(head.x,head.y,head.z,Block_Types::Air))
            invalidate_chunk_and_neighbors(head.x,head.y,head.z,manager,renderer);
    } else if (type==Block_Types::Piston_Head) {
        const glm::ivec3 base=at-facing[meta&3u];
        const Block_Types base_type=manager.get_block_world(base.x,base.y,base.z);
        if (base_type!=Block_Types::Piston && base_type!=Block_Types::Sticky_Piston) return;
        const uint8_t base_meta=manager.get_block_meta_world(base.x,base.y,base.z);
        if ((base_meta&3u)!=(meta&3u) || (base_meta&Block_Meta::active_bit)==0u) return;
        if (manager.set_block_world(base.x,base.y,base.z,base_type,
                                    static_cast<uint8_t>(base_meta&~Block_Meta::active_bit)))
            invalidate_chunk_and_neighbors(base.x,base.y,base.z,manager,renderer);
    }
}
}

Block_Interaction::Block_Interaction() = default;

std::vector<glm::ivec3> Block_Interaction::explode(const glm::ivec3& center, float radius,
                                                   Chunk_Manager& chunk_manager, Renderer& renderer) {
    static std::mt19937 s_rng(std::random_device{}());
    std::uniform_real_distribution<float> jitter(0.7f, 1.3f);
    constexpr int k_rays = 16;          // лучи идут к граням куба 16x16x16 (поверхность сферы)
    constexpr float k_step = 0.3f;
    const glm::vec3 origin = glm::vec3(center) + glm::vec3(0.5f);
    std::set<std::array<int, 3>> marked;

    for (int i = 0; i <= k_rays; ++i) {
        for (int j = 0; j <= k_rays; ++j) {
            for (int k = 0; k <= k_rays; ++k) {
                const bool on_surface = i == 0 || i == k_rays || j == 0 || j == k_rays || k == 0 || k == k_rays;
                if (!on_surface) continue;
                const glm::vec3 dir = glm::normalize(glm::vec3(i, j, k) / static_cast<float>(k_rays) * 2.0f - 1.0f);
                float energy = radius * jitter(s_rng);
                glm::vec3 pos = origin;
                while (energy > 0.0f) {
                    pos += dir * k_step;
                    const glm::ivec3 b = glm::floor(pos);
                    if (b.y < Config::world_min_y || b.y > Config::world_max_y) break;
                    const Block_Types type = chunk_manager.get_block_world(b.x, b.y, b.z);
                    if (type != Block_Types::Air) {
                        const Block_Properties& props = get_block_props(type);
                        if (!props.unbreakable()) marked.insert({b.x, b.y, b.z});
                        energy -= (std::max(0.0f, props.blast_resistance) + 0.3f) * k_step;
                    }
                    energy -= 0.225f;
                }
            }
        }
    }

    std::vector<glm::ivec3> destroyed;
    for (const auto& m : marked) {
        const glm::ivec3 b(m[0], m[1], m[2]);
        const Block_Types type = chunk_manager.get_block_world(b.x, b.y, b.z);
        if (type == Block_Types::Air) continue; // a previous door half may have cleared this cell
        const uint8_t meta = chunk_manager.get_block_meta_world(b.x, b.y, b.z);
        if (chunk_manager.set_block_world(b.x, b.y, b.z, Block_Types::Air)) {
            remove_door_companion(b, type, meta, chunk_manager, renderer);
            remove_piston_companion(b, type, meta, chunk_manager, renderer);
            invalidate_chunk_and_neighbors(b.x, b.y, b.z, chunk_manager, renderer);
            destroyed.push_back(b);
        }
    }
    return destroyed;
}

bool Block_Interaction::can_harvest(Block_Types block, const Item_Stack& held) {
    const Block_Properties& props = get_block_props(block);
    if (props.harvest_tier <= 0) return true;
    if (held.is_empty()) return false;
    const Block_Properties& tool = get_block_props(held.type);
    return tool.tool_type == Tool_Type::Pickaxe && tool.tool_tier >= props.harvest_tier;
}

float Block_Interaction::get_survival_break_time(Block_Types block, const Item_Stack& held) {
    const Block_Properties& props = get_block_props(block);
    float time = props.break_time >= 0.0f
        ? props.break_time
        : std::max(0.0f, props.hardness) * Config::survival_break_seconds_per_hardness;
    if (!held.is_empty()) {
        const Block_Properties& tool = get_block_props(held.type);
        if (tool.tool_type != Tool_Type::None && tool.tool_type == props.effective_tool) {
            time /= std::max(1.0f, tool.tool_speed) * (1.0f + 0.35f*held.enchant_efficiency*held.enchant_efficiency);
        }
    }
    // Руду без нужной кирки ломать долго (и дропа не будет).
    if (props.harvest_tier > 0 && !can_harvest(block, held)) time *= 3.0f;
    return time;
}

void Block_Interaction::cancel_actions() {
    m_is_mining = false;
    m_mining_time = 0.0f;
    m_break_progress = 0.0f;
    m_eat_timer = 0.0f;
    m_eat_progress = 0.0f;
    m_eating_type = Block_Types::Air;
}

void Block_Interaction::update(float delta_time,
                               const glm::vec3& eye_position,
                               const glm::vec3& look_direction,
                               const Input_Manager& input,
                               Player& player,
                               Chunk_Manager& chunk_manager,
                               Renderer& renderer,
                               Inventory& inventory,
                               float entity_hit_distance) {
    m_action_cooldown = std::max(0.0f, m_action_cooldown - delta_time);
    m_current_hit = Raycast::cast(eye_position, look_direction, k_max_reach, chunk_manager);

    // Моб ближе первого блока на луче — он перехватывает взгляд.
    m_entity_targeted = entity_hit_distance >= 0.0f &&
                        (!m_current_hit.hit || entity_hit_distance < m_current_hit.distance);
    if (m_entity_targeted) {
        m_current_hit.hit = false;
    }

    const bool survival = inventory.get_mode() == Game_Mode::Survival;

    // --- Еда (Survival): ПКМ удерживается на съедобном предмете в активном слоте -------------
    // Работает и когда игрок смотрит в небо (нет блока на луче), поэтому до проверки цели.
    {
        const Item_Stack held = inventory.get_hotbar_slot(static_cast<size_t>(inventory.get_selected_index()));
        const Block_Properties& held_props = get_block_props(held.type);
        const bool aimed_at_container = m_current_hit.hit &&
            block_entity_type_for(m_current_hit.block_type) != Block_Entity_Type::None;
        // Морковь на грядке сажается, а не съедается; моб на луче кормится (см. Game).
        const bool aimed_at_farmland = m_current_hit.hit && held.type == Block_Types::Carrot &&
            Crop_Manager::is_farmland(m_current_hit.block_type);
        const bool wants_to_eat = survival && !held.is_empty() && held_props.food_points > 0 &&
                                  player.can_eat() && player.is_alive() && !aimed_at_container &&
                                  !aimed_at_farmland && !m_entity_targeted &&
                                  input.is_mouse_button_held(MOUSE_BUTTON_RIGHT);

        if (wants_to_eat && (m_eat_timer == 0.0f || m_eating_type == held.type)) {
            m_eating_type = held.type;
            m_eat_timer += delta_time;
            m_eat_progress = std::clamp(m_eat_timer / Config::eat_duration, 0.0f, 1.0f);
            if (m_eat_timer >= Config::eat_duration) {
                if (inventory.remove_resource(held.type, 1)) {
                    player.eat(held_props.food_points, held_props.food_saturation);
                }
                // Следующая порция начинается заново — как в Minecraft, нужно снова держать ПКМ.
                m_eat_timer = 0.0f;
                m_eat_progress = 0.0f;
                m_eating_type = Block_Types::Air;
                m_action_cooldown = k_action_delay;
            }
        } else {
            m_eat_timer = 0.0f;
            m_eat_progress = 0.0f;
            m_eating_type = Block_Types::Air;
        }
    }

    // Пока ест — блоки не ломаем и не ставим.
    if (m_eat_progress > 0.0f) {
        m_is_mining = false;
        m_break_progress = 0.0f;
        return;
    }

    // --- Добыча ------------------------------------------------------------------------------
    const bool left_held = input.is_mouse_button_held(MOUSE_BUTTON_LEFT);

    if (survival) {
        // Смотрим на другой блок (или никуда) / отпустили кнопку — прогресс сбрасывается,
        // как в Minecraft: недокопанный блок «зарастает».
        const bool same_target = m_current_hit.hit &&
            m_mining_pos == glm::ivec3(m_current_hit.block_x, m_current_hit.block_y, m_current_hit.block_z) &&
            m_mining_type == m_current_hit.block_type;
        if (!left_held || !m_current_hit.hit || !same_target) {
            m_is_mining = false;
            m_mining_time = 0.0f;
            m_break_progress = 0.0f;
        }

        if (left_held && m_current_hit.hit && m_action_cooldown <= 0.0f &&
            !get_block_props(m_current_hit.block_type).unbreakable()) {
            if (!m_is_mining) {
                m_is_mining = true;
                m_mining_pos = glm::ivec3(m_current_hit.block_x, m_current_hit.block_y, m_current_hit.block_z);
                m_mining_type = m_current_hit.block_type;
                m_mining_time = 0.0f;
            }
            m_mining_time += delta_time;

            const Item_Stack held_stack = inventory.get_selected_stack();
            const float required = get_survival_break_time(m_mining_type, held_stack);
            m_break_progress = required > 0.0f ? std::clamp(m_mining_time / required, 0.0f, 1.0f) : 1.0f;

            if (m_mining_time >= required) {
                Broken_Block_Event event;
                event.position = m_mining_pos;
                event.type = m_mining_type;
                event.harvested = can_harvest(m_mining_type, held_stack);
                event.fortune_level = held_stack.enchant_fortune;
                // Содержимое сундука/печи/верстака снимаем сейчас: set_block_world ниже удалит
                // Block_Entity вместе с блоком. Своя половина двойного сундука — только своя.
                if (const Block_Entity* entity = chunk_manager.get_block_entity(
                        m_mining_pos.x, m_mining_pos.y, m_mining_pos.z)) {
                    for (const Item_Stack& stack : entity->slots) {
                        if (!stack.is_empty()) event.container_contents.push_back(stack);
                    }
                }

                const uint8_t broken_meta = chunk_manager.get_block_meta_world(
                    m_mining_pos.x, m_mining_pos.y, m_mining_pos.z);
                if (chunk_manager.set_block_world(m_mining_pos.x, m_mining_pos.y, m_mining_pos.z,
                                                  Block_Types::Air)) {
                    remove_door_companion(m_mining_pos, m_mining_type, broken_meta, chunk_manager, renderer);
                    remove_piston_companion(m_mining_pos, m_mining_type, broken_meta, chunk_manager, renderer);
                    m_broken_blocks.push_back(std::move(event));
                    player.add_exhaustion(Config::exhaustion_break_block);
                    // Инструмент в руке изнашивается (на добыче «пустышек» вроде травы — нет).
                    if (get_block_props(m_mining_type).hardness > 0.0f) {
                        inventory.damage_selected_tool(1);
                    }
                    invalidate_chunk_and_neighbors(m_mining_pos.x, m_mining_pos.y, m_mining_pos.z,
                                                   chunk_manager, renderer);
                }
                m_is_mining = false;
                m_mining_time = 0.0f;
                m_break_progress = 0.0f;
                m_action_cooldown = k_break_cooldown;
                m_current_hit.hit = false;
                return;
            }
        }
    } else {
        m_is_mining = false;
        m_break_progress = 0.0f;
    }

    if (!m_current_hit.hit || m_action_cooldown > 0.0f) {
        return;
    }

    // Creative: мгновенно ломает любой блок, включая блоки с отрицательной прочностью,
    // но не удаляет жидкости. Событие ниже очищает служебное состояние растений и логики,
    // не создавая дроп — в Creative предметы из сломанных блоков не выпадают.
    if (!survival && input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT)) {
        const Block_Properties& target_props = get_block_props(m_current_hit.block_type);
        if (target_props.state == Block_State::Liquid) {
            // Жидкость остаётся нетронутой; не запускаем повторное разрушение на следующем кадре.
            m_action_cooldown = k_action_delay;
            return;
        }

        const glm::ivec3 broken_pos(m_current_hit.block_x, m_current_hit.block_y, m_current_hit.block_z);
        const Block_Types broken_type = m_current_hit.block_type;
        const uint8_t broken_meta = chunk_manager.get_block_meta_world(
            broken_pos.x, broken_pos.y, broken_pos.z);
        if (!chunk_manager.set_block_world(
            broken_pos.x, broken_pos.y, broken_pos.z, Block_Types::Air)) {
            return;
        }

        remove_door_companion(broken_pos, broken_type, broken_meta, chunk_manager, renderer);
        remove_piston_companion(broken_pos, broken_type, broken_meta, chunk_manager, renderer);
        Broken_Block_Event event;
        event.position = broken_pos;
        event.type = broken_type;
        event.harvested = false;
        event.allow_drops = false;
        m_broken_blocks.push_back(std::move(event));

        invalidate_chunk_and_neighbors(
            broken_pos.x, broken_pos.y, broken_pos.z, chunk_manager, renderer);
        m_action_cooldown = k_action_delay;
        m_current_hit.hit = false;
        return;
    }

    if (input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT)) {
        // Блок, на который сейчас смотрит игрок (а не клетка, куда встал бы новый блок) —
        // если это контейнер, ПКМ его открывает, как в Minecraft, а не ставит блок ему на грань.
        if (block_entity_type_for(m_current_hit.block_type) != Block_Entity_Type::None) {
            m_container_open_request = glm::ivec3(
                m_current_hit.block_x, m_current_hit.block_y, m_current_hit.block_z);
            m_action_cooldown = k_action_delay;
            m_current_hit.hit = false;
            return;
        }

        // Рычаг, кнопка, зажиматель, повторитель: ПКМ переключает/меняет, блок не ставится.
        // Провод — нет: по нему ставят другие блоки.
        if (Logic_Manager::is_interactable(m_current_hit.block_type)) {
            m_redstone_interact = glm::ivec3(m_current_hit.block_x, m_current_hit.block_y, m_current_hit.block_z);
            m_action_cooldown = k_action_delay;
            m_current_hit.hit = false;
            return;
        }

        // Двери, калитки и люки переключаются по ПКМ: bit 2 of meta = open.
        // Для дверей переключаются обе половины атомарно по отношению к их сохранённым координатам.
        const Block_Shape clicked_shape = get_block_props(m_current_hit.block_type).shape;
        if (clicked_shape == Block_Shape::Door || clicked_shape == Block_Shape::FenceGate ||
            clicked_shape == Block_Shape::Trapdoor) {
            const glm::ivec3 at(m_current_hit.block_x,m_current_hit.block_y,m_current_hit.block_z);
            const uint8_t meta = chunk_manager.get_block_meta_world(at.x,at.y,at.z);
            const uint8_t new_meta = static_cast<uint8_t>(meta ^ 4u);
            if (chunk_manager.set_block_world(at.x,at.y,at.z,m_current_hit.block_type,new_meta)) {
                invalidate_chunk_and_neighbors(at.x,at.y,at.z,chunk_manager,renderer);
                if (clicked_shape == Block_Shape::Door) {
                    const int other_y = at.y + ((meta & 8u) != 0u ? -1 : 1);
                    if (chunk_manager.get_block_world(at.x,other_y,at.z)==m_current_hit.block_type) {
                        const uint8_t other_meta=chunk_manager.get_block_meta_world(at.x,other_y,at.z);
                        if (((other_meta ^ meta) & 8u) != 0u &&
                            chunk_manager.set_block_world(at.x,other_y,at.z,m_current_hit.block_type,
                                                           static_cast<uint8_t>(other_meta^4u)))
                            invalidate_chunk_and_neighbors(at.x,other_y,at.z,chunk_manager,renderer);
                    }
                }
                m_action_cooldown=k_action_delay;
            }
            return;
        }

        // --- Земледелие -----------------------------------------------------------------------
        {
            const Block_Properties& held_props = get_block_props(m_place_block);
            const glm::ivec3 target(m_current_hit.block_x, m_current_hit.block_y, m_current_hit.block_z);
            const glm::ivec3 above = target + glm::ivec3(0, 1, 0);
            const bool top_face = m_current_hit.place_x == above.x && m_current_hit.place_y == above.y &&
                                  m_current_hit.place_z == above.z;
            const Block_Types above_block = chunk_manager.get_block_world(above.x, above.y, above.z);
            const bool above_free = above_block == Block_Types::Air ||
                                    (!get_block_props(above_block).is_solid &&
                                     get_block_props(above_block).state != Block_State::Liquid &&
                                     !Crop_Manager::is_crop_block(above_block));

            // Мотыга: трава/земля -> грядка.
            if (held_props.tool_type == Tool_Type::Hoe &&
                (m_current_hit.block_type == Block_Types::Grass || m_current_hit.block_type == Block_Types::Dirt) &&
                above_free) {
                const Block_Types farmland = Crop_Manager::has_water_nearby(chunk_manager, target)
                    ? Block_Types::Farmland_Wet : Block_Types::Farmland;
                if (chunk_manager.set_block_world(target.x, target.y, target.z, farmland)) {
                    // Трава над грядкой (цветок/травинка) сносится, иначе висела бы в воздухе.
                    if (above_block != Block_Types::Air) {
                        chunk_manager.set_block_world(above.x, above.y, above.z, Block_Types::Air);
                    }
                    invalidate_chunk_and_neighbors(target.x, target.y, target.z, chunk_manager, renderer);
                    if (survival) inventory.damage_selected_tool(1);
                    m_action_cooldown = k_action_delay;
                }
                return;
            }

            // Семена / морковь: посадка на грядку сверху.
            const bool plantable_item = m_place_block == Block_Types::Wheat_Seeds || m_place_block == Block_Types::Carrot;
            if (plantable_item && m_crop_manager &&
                (m_current_hit.block_type == Block_Types::Farmland || m_current_hit.block_type == Block_Types::Farmland_Wet)) {
                if (top_face && above_free && !player.intersects_block_aabb(above.x, above.y, above.z)) {
                    const Crop_Kind kind = m_place_block == Block_Types::Carrot ? Crop_Kind::Carrot : Crop_Kind::Wheat;
                    if (chunk_manager.set_block_world(above.x, above.y, above.z, Crop_Manager::crop_block(kind, 0))) {
                        m_crop_manager->plant(above, kind);
                        if (survival) inventory.consume_selected(1);
                        invalidate_chunk_and_neighbors(above.x, above.y, above.z, chunk_manager, renderer);
                        m_action_cooldown = k_action_delay;
                    }
                }
                return;
            }
        }

        // Предметы (мясо, уголь) — не блоки: в мир они не ставятся.
        if (m_place_block == Block_Types::Air || get_block_props(m_place_block).is_item) {
            return;
        }

        // Саженец ели ставится только сверху на землю или дёрн.
        if (Sapling_Manager::is_sapling(m_place_block)) {
            const bool on_top = m_current_hit.place_x == m_current_hit.block_x &&
                                m_current_hit.place_y == m_current_hit.block_y + 1 &&
                                m_current_hit.place_z == m_current_hit.block_z;
            if (!on_top || !Sapling_Manager::can_grow_on(m_current_hit.block_type)) {
                return;
            }
        }

        const glm::vec3 place_pos(
            static_cast<float>(m_current_hit.place_x),
            static_cast<float>(m_current_hit.place_y),
            static_cast<float>(m_current_hit.place_z)
        );

        const float dx = eye_position.x - place_pos.x;
        const float dy = eye_position.y - place_pos.y;
        const float dz = eye_position.z - place_pos.z;
        const float dist_sq = dx * dx + dy * dy + dz * dz;

        if (dist_sq > 1.75f * 1.75f &&
            !player.intersects_block_aabb(
                m_current_hit.place_x, m_current_hit.place_y, m_current_hit.place_z)) {
            // Survival: нечего ставить — пустой/несовпадающий слот хотбара.
            if (survival && !inventory.has_resource(m_place_block, 1)) {
                return;
            }
            // Метаданные при постановке: бревно ложится вдоль оси грани, на которую его
            // поставили (на бок соседнего блока -> ствол горизонтально), а печь/сундук/верстак
            // разворачиваются лицом к игроку.
            uint8_t placed_meta = 0;
            const Block_Properties& place_props = get_block_props(m_place_block);
            if (place_props.orientation == Block_Orientation::Axis) {
                const int nx = m_current_hit.place_x - m_current_hit.block_x;
                const int nz = m_current_hit.place_z - m_current_hit.block_z;
                if (nx != 0)      placed_meta = Block_Meta::axis_x;
                else if (nz != 0) placed_meta = Block_Meta::axis_z;
                else              placed_meta = Block_Meta::axis_y;
            } else if (place_props.orientation == Block_Orientation::Facing) {
                const float to_player_x = eye_position.x - (place_pos.x + 0.5f);
                const float to_player_z = eye_position.z - (place_pos.z + 0.5f);
                if (std::abs(to_player_x) > std::abs(to_player_z)) {
                    placed_meta = to_player_x > 0.0f ? Block_Meta::facing_east : Block_Meta::facing_west;
                } else {
                    placed_meta = to_player_z > 0.0f ? Block_Meta::facing_south : Block_Meta::facing_north;
                }
                // Ступеньки: высокая сторона — от игрока (по ней идут вверх, к игроку — вниз).
                if (place_props.shape == Block_Shape::Stairs) {
                    placed_meta = opposite_facing(placed_meta);
                }
            }

            // Тонкие напольные формы должны иметь опору снизу, а лестница крепится
            // строго к боковой грани твёрдого блока (meta задаёт грань крепления).
            if (place_props.shape == Block_Shape::Ladder) {
                const int sx = m_current_hit.block_x - m_current_hit.place_x;
                const int sy = m_current_hit.block_y - m_current_hit.place_y;
                const int sz = m_current_hit.block_z - m_current_hit.place_z;
                if (sy != 0 || (std::abs(sx) + std::abs(sz)) != 1 ||
                    !get_block_props(m_current_hit.block_type).is_solid) return;
                if (sx != 0) placed_meta = sx > 0 ? Block_Meta::facing_east : Block_Meta::facing_west;
                else placed_meta = sz > 0 ? Block_Meta::facing_south : Block_Meta::facing_north;
            }
            if (place_props.shape == Block_Shape::PressurePlate || place_props.shape == Block_Shape::Carpet ||
                place_props.shape == Block_Shape::Rail || place_props.shape == Block_Shape::SnowLayer) {
                if (m_current_hit.place_y != m_current_hit.block_y + 1 ||
                    !get_block_props(m_current_hit.block_type).is_solid) return;
            }

            // Красная логика: рычаг/кнопка/зажиматель — на грань опоры, повторитель — выходом от игрока,
            // провод — на твёрдую опору снизу. Без опоры не ставится (см. Logic_Manager::placement).
            const bool is_redstone_node = Logic_Manager::is_node(m_place_block);
            if (is_redstone_node) {
                const float to_player_x = eye_position.x - (place_pos.x + 0.5f);
                const float to_player_z = eye_position.z - (place_pos.z + 0.5f);
                uint8_t toward_player = Block_Meta::facing_north;
                if (std::abs(to_player_x) > std::abs(to_player_z)) {
                    toward_player = to_player_x > 0.0f ? Block_Meta::facing_east : Block_Meta::facing_west;
                } else {
                    toward_player = to_player_z > 0.0f ? Block_Meta::facing_south : Block_Meta::facing_north;
                }
                auto world = make_chunk_world(chunk_manager);
                uint8_t node_meta = 0;
                if (!Logic_Manager::placement(
                        m_place_block, world,
                        glm::ivec3(m_current_hit.place_x, m_current_hit.place_y, m_current_hit.place_z),
                        glm::ivec3(m_current_hit.block_x, m_current_hit.block_y, m_current_hit.block_z),
                        opposite_facing(toward_player), node_meta)) {
                    return;
                }
                placed_meta = node_meta;
            }

            const bool placing_door = place_props.shape == Block_Shape::Door;
            if (placing_door) {
                const int upper_y = m_current_hit.place_y + 1;
                if (upper_y > Config::world_max_y ||
                    chunk_manager.get_block_world(m_current_hit.place_x,upper_y,m_current_hit.place_z) != Block_Types::Air ||
                    player.intersects_block_aabb(m_current_hit.place_x, upper_y, m_current_hit.place_z)) {
                    return;
                }
            }
            if (!chunk_manager.set_block_world(
                m_current_hit.place_x,
                m_current_hit.place_y,
                m_current_hit.place_z,
                m_place_block,
                placed_meta
            )) {
                return;
            }
            if (placing_door) {
                // Unlike two independently placed doors, upper/lower halves share one item.
                if (!chunk_manager.set_block_world(m_current_hit.place_x, m_current_hit.place_y + 1,
                        m_current_hit.place_z, m_place_block, static_cast<uint8_t>(placed_meta | 8u))) {
                    chunk_manager.set_block_world(m_current_hit.place_x, m_current_hit.place_y,
                                                  m_current_hit.place_z, Block_Types::Air);
                    return;
                }
                invalidate_chunk_and_neighbors(m_current_hit.place_x, m_current_hit.place_y+1,
                                               m_current_hit.place_z,chunk_manager,renderer);
            }
            if (is_redstone_node) {
                m_redstone_placed = glm::ivec3(m_current_hit.place_x, m_current_hit.place_y, m_current_hit.place_z);
            }
            if (survival) {
                inventory.remove_resource(m_place_block, 1);
            }
            if (m_sapling_manager && Sapling_Manager::is_sapling(m_place_block)) {
                m_sapling_manager->plant(glm::ivec3(m_current_hit.place_x, m_current_hit.place_y, m_current_hit.place_z),
                                         m_place_block);
            }
            invalidate_chunk_and_neighbors(
                m_current_hit.place_x, m_current_hit.place_y, m_current_hit.place_z,
                chunk_manager, renderer);
            m_action_cooldown = k_action_delay;
        }
    }
}
