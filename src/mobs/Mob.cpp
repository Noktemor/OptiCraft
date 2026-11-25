#include "mobs/Mob.h"
#include "world/chunks/Chunk_Manager.h"
#include "utils/logging/Logger.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <unordered_map>
#include <cctype>
#include <cmath>
#include <string>
#include <glm/gtc/type_ptr.hpp>

namespace {
    std::string to_lower_ascii(std::string text) {
        for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }
}

namespace {

glm::vec3 sample_vec3(const std::vector<Anim_Key<glm::vec3>>& keys, float time, const glm::vec3& fallback) {
    if (keys.empty()) return fallback;
    if (keys.size() == 1 || time <= keys.front().time) return keys.front().value;
    if (time >= keys.back().time) return keys.back().value;

    for (size_t i = 0; i + 1 < keys.size(); ++i) {
        if (time >= keys[i].time && time <= keys[i + 1].time) {
            const float span = keys[i + 1].time - keys[i].time;
            const float t = span > 0.0001f ? (time - keys[i].time) / span : 0.0f;
            return glm::mix(keys[i].value, keys[i + 1].value, t);
        }
    }
    return keys.back().value;
}

glm::quat sample_quat(const std::vector<Anim_Key<glm::quat>>& keys, float time, const glm::quat& fallback) {
    if (keys.empty()) return fallback;
    if (keys.size() == 1 || time <= keys.front().time) return keys.front().value;
    if (time >= keys.back().time) return keys.back().value;

    for (size_t i = 0; i + 1 < keys.size(); ++i) {
        if (time >= keys[i].time && time <= keys[i + 1].time) {
            const float span = keys[i + 1].time - keys[i].time;
            const float t = span > 0.0001f ? (time - keys[i].time) / span : 0.0f;
            return glm::slerp(keys[i].value, keys[i + 1].value, t);
        }
    }
    return keys.back().value;
}

} // namespace

namespace {
// static locals имеют static storage duration — живут до конца процесса,
// если их явно не почистить (см. Mob::clear_model_cache и предупреждение в Mob.h).
std::unordered_map<std::string, std::shared_ptr<Mob_Model>>& model_cache() {
    static std::unordered_map<std::string, std::shared_ptr<Mob_Model>> cache;
    return cache;
}
} // namespace

bool Mob::load(const std::string& glb_path) {
    // Общий кэш моделей по пути к файлу: у 50 зомби на карте — одна Mob_Model
    // и одни GPU-буферы на всех, у каждого Mob своё только состояние анимации.
    auto& cache = model_cache();

    if (const auto it = cache.find(glb_path); it != cache.end()) {
        m_model = it->second;
        return m_model->is_loaded();
    }

    auto model = std::make_shared<Mob_Model>();
    if (!model->load_from_file(glb_path)) {
        return false;
    }
    cache[glb_path] = model;
    m_model = model;
    return true;
}

void Mob::clear_model_cache() {
    model_cache().clear();
}

void Mob::play_animation(const std::string& name, bool loop) {
    if (m_current_animation == name) return; // уже играет — не сбрасываем время

    if (!m_model || !m_model->has_animation(name)) {
        LOG_WARN("Mob::play_animation: animation '" + name + "' not found");
        return;
    }

    m_current_animation = name;
    m_animation_time = 0.0f;
    m_loop = loop;
}

bool Mob::is_animation_finished() const {
    if (m_loop || m_current_animation.empty() || !m_model) return false;
    const Animation_Clip* clip = m_model->find_animation(m_current_animation);
    return clip && m_animation_time >= clip->duration;
}

void Mob::update(float delta_time) {
    if (!m_model || m_current_animation.empty()) return;

    const Animation_Clip* clip = m_model->find_animation(m_current_animation);
    if (!clip || clip->duration <= 0.0f) return;

    m_animation_time += delta_time * m_animation_speed;
    if (m_animation_time > clip->duration) {
        m_animation_time = m_loop ? std::fmod(m_animation_time, clip->duration) : clip->duration;
    }
}

void Mob::compute_bone_matrices(std::vector<glm::mat4>& out_skin_matrices) const {
    const Animation_Clip* clip = m_current_animation.empty() ? nullptr : m_model->find_animation(m_current_animation);
    compute_bone_matrices(out_skin_matrices, clip, m_animation_time);
}

void Mob::compute_bone_matrices(std::vector<glm::mat4>& out_skin_matrices, const Animation_Clip* clip, float time) const {
    const std::vector<Mob_Bone>& bones = m_model->get_bones();
    out_skin_matrices.assign(bones.size(), glm::mat4(1.0f));
    if (bones.empty()) return;

    // Рабочие массивы переиспользуются между вызовами (на каждого моба каждый кадр раньше — 2 аллокации).
    thread_local std::vector<glm::mat4> global_matrices;
    global_matrices.assign(bones.size(), glm::mat4(1.0f));

    // Канал клипа на кость за один проход по каналам (берётся ПЕРВЫЙ найденный, как и раньше):
    // вместо поиска по всем каналам для каждой кости — O(кости × каналы) -> O(каналы).
    thread_local std::vector<const Bone_Animation_Channel*> channel_of_bone;
    channel_of_bone.assign(bones.size(), nullptr);
    if (clip) {
        for (const Bone_Animation_Channel& channel : clip->channels) {
            if (channel.bone_index < 0 || static_cast<size_t>(channel.bone_index) >= bones.size()) continue;
            if (!channel_of_bone[channel.bone_index]) channel_of_bone[channel.bone_index] = &channel;
        }
    }

    // Скрытые кости (см. set_hidden_bones_matching): совпадение по имени и все потомки.
    // Порядок eval_order гарантирует, что родитель уже помечен, когда доходим до потомка.
    std::vector<bool> hidden(bones.size(), false);
    if (!m_hidden_bone_token.empty()) {
        const std::string token = to_lower_ascii(m_hidden_bone_token);
        for (const int bone_index : m_model->get_bone_eval_order()) {
            const Mob_Bone& bone = bones[bone_index];
            const bool parent_hidden = bone.parent_index >= 0 && hidden[bone.parent_index];
            hidden[bone_index] = parent_hidden || to_lower_ascii(bone.name).find(token) != std::string::npos;
        }
    }

    for (const int bone_index : m_model->get_bone_eval_order()) {
        const Mob_Bone& bone = bones[bone_index];

        glm::vec3 translation = bone.bind_translation;
        glm::quat rotation = bone.bind_rotation;
        glm::vec3 scale = bone.bind_scale;

        if (const Bone_Animation_Channel* channel_ptr = channel_of_bone[bone_index]) {
            const Bone_Animation_Channel& channel = *channel_ptr;
            if (!channel.translation_keys.empty()) translation = sample_vec3(channel.translation_keys, time, translation);
            if (!channel.rotation_keys.empty()) rotation = sample_quat(channel.rotation_keys, time, rotation);
            if (!channel.scale_keys.empty()) scale = sample_vec3(channel.scale_keys, time, scale);
        }

        const glm::mat4 local_matrix = glm::translate(glm::mat4(1.0f), translation) * glm::mat4_cast(rotation) * glm::scale(glm::mat4(1.0f), scale);
        // Для корневых костей — не identity, а полная трансформация предков
        // выше skin.joints (см. Mob_Bone::root_parent_transform). Для
        // остальных — обычное наследование через уже посчитанного родителя.
        const glm::mat4 parent_global = bone.parent_index >= 0 ? global_matrices[bone.parent_index] : bone.root_parent_transform;

        global_matrices[bone_index] = parent_global * local_matrix;
        if (hidden[bone_index]) {
            // Масштаб 0 в системе кости (до global): вершины схлопываются в сустав, а не в начало мира.
            out_skin_matrices[bone_index] = global_matrices[bone_index] *
                glm::scale(glm::mat4(1.0f), glm::vec3(0.0f)) * bone.inverse_bind_matrix;
        } else {
            out_skin_matrices[bone_index] = global_matrices[bone_index] * bone.inverse_bind_matrix;
        }
    }
}

namespace {thread_local const Mob::EntitySink* entitySink=nullptr;}
Mob::ScopedEntitySink::ScopedEntitySink(const EntitySink& sink):previous_(entitySink){entitySink=&sink;}
Mob::ScopedEntitySink::~ScopedEntitySink(){entitySink=previous_;}

void Mob::render(const Shader& shader, const glm::mat4& view, const glm::mat4& projection, float ambient_intensity) const {
    if (!is_loaded()) return;

    std::vector<glm::mat4> skin_matrices;
    compute_bone_matrices(skin_matrices);
    const bool has_skin = !skin_matrices.empty();
    if (!has_skin) skin_matrices.assign(1, glm::mat4(1.0f)); // шейдер всё равно ждёт непустой массив

    glm::mat4 model_matrix(1.0f);
    model_matrix = glm::translate(model_matrix, m_position);
    model_matrix = glm::rotate(model_matrix, glm::radians(m_yaw_degrees), glm::vec3(0.0f, 1.0f, 0.0f));
    if (m_roll_degrees != 0.0f) {
        model_matrix = glm::translate(model_matrix, glm::vec3(0.0f, m_roll_pivot_y, 0.0f));
        model_matrix = glm::rotate(model_matrix, glm::radians(m_roll_degrees), glm::vec3(0.0f, 0.0f, 1.0f));
        model_matrix = glm::translate(model_matrix, glm::vec3(0.0f, -m_roll_pivot_y, 0.0f));
    }
    model_matrix = glm::scale(model_matrix, glm::vec3(m_scale));

    if(entitySink){
        OptiCraft::Vulkan::EntityDraw draw;
        draw.asset=m_model->vulkan_asset();
        std::copy_n(glm::value_ptr(model_matrix),16,draw.pose.model.begin());
        const auto count=has_skin?std::min(skin_matrices.size(),size_t(OptiCraft::Vulkan::EntityBones)):0;
        for(size_t i=0;i<count;++i)std::copy_n(glm::value_ptr(skin_matrices[i]),16,draw.pose.bones[i].begin());
        draw.pose.options={float(count),m_hurt_flash,ambient_intensity,0.f};
        draw.hiddenMaterial=m_hidden_material_token;
        (*entitySink)(std::move(draw));return;
    }
    shader.use();
    shader.set_mat4("model", model_matrix);
    shader.set_mat4("view", view);
    shader.set_mat4("projection", projection);
    shader.set_int("has_skin", has_skin ? 1 : 0);
    shader.set_mat4_array("bones", skin_matrices);
    shader.set_float("ambient", ambient_intensity);
    shader.set_float("hurt_flash", m_hurt_flash);

    if (Texture* albedo = m_model->get_texture("albedo")) {
        albedo->bind(0);
        shader.set_int("albedo_texture", 0);
    }

    glBindVertexArray(m_model->get_vao());
    const auto& ranges = m_model->get_primitive_ranges();
    if (ranges.empty()) {
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(m_model->get_index_count()), GL_UNSIGNED_INT, nullptr);
    } else {
        for (const auto& range : ranges) {
            if (!m_hidden_material_token.empty() &&
                range.material_name.find(m_hidden_material_token) != std::string::npos) continue;
            Texture* tex = m_model->get_texture(range.texture_role);
            if (!tex) tex = m_model->get_texture("albedo");
            if (tex) { tex->bind(0); shader.set_int("albedo_texture", 0); }
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(range.index_count), GL_UNSIGNED_INT,
                           reinterpret_cast<const void*>(range.first_index * sizeof(uint32_t)));
        }
    }
    glBindVertexArray(0);
}

bool Mob::get_local_bounds(glm::vec3& out_min, glm::vec3& out_max) const {
    if (!is_loaded()) return false;

    if (!m_model->has_bounds()) {
        const std::vector<Mob_Vertex>& vertices = m_model->get_cpu_vertices();
        if (vertices.empty()) return false;

        // Габариты берём в позе Idle в t=0 (если такого клипа нет — в bind-позе): в ней моб стоит
        // ровно, а размах ног/головы при ходьбе хитбокс не раздувает.
        const Animation_Clip* pose = m_model->find_animation("Idle");
        std::vector<glm::mat4> skin;
        compute_bone_matrices(skin, pose, 0.0f);
        const bool has_skin = !skin.empty();

        // Защита от битых моделей: вершины, которые скиннинг унёс дальше k_max_extent блоков от
        // начала координат моба (сломанный экспорт FBX->glTF, см. предупреждения «bind-pose
        // sanity check» при загрузке), в габариты не входят — иначе хитбокс стал бы огромным.
        constexpr float k_max_extent = 8.0f;
        int used_vertices = 0;
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (const Mob_Vertex& v : vertices) {
            glm::vec4 p(v.position, 1.0f);
            if (has_skin) {
                glm::mat4 m(0.0f);
                for (int i = 0; i < 4; ++i) {
                    const float w = v.joint_weights[i];
                    const int j = v.joint_indices[i];
                    if (w > 0.0f && j >= 0 && j < static_cast<int>(skin.size())) m += skin[j] * w;
                }
                p = m * p;
            }
            const glm::vec3 q(p);
            if (std::abs(q.x) > k_max_extent || std::abs(q.y) > k_max_extent || std::abs(q.z) > k_max_extent) continue;
            lo = glm::min(lo, q);
            hi = glm::max(hi, q);
            ++used_vertices;
        }
        if (used_vertices < 8) return false;
        if (!(lo.x < hi.x && lo.y < hi.y && lo.z < hi.z)) return false;
        m_model->set_bounds(lo, hi);
        LOG_INFO("Mob: model bounds min(" + std::to_string(lo.x) + ", " + std::to_string(lo.y) + ", " + std::to_string(lo.z) +
                 ") max(" + std::to_string(hi.x) + ", " + std::to_string(hi.y) + ", " + std::to_string(hi.z) + ")");
    }

    out_min = m_model->get_bounds_min();
    out_max = m_model->get_bounds_max();
    return true;
}

Mob_Hit_Result Mob::raycast_body_part(const glm::vec3& origin, const glm::vec3& dir, float max_distance) const {
    Mob_Hit_Result best;
    if (!is_loaded()) return best;
    // Very cheap broad phase: for most mobs no animation/bone evaluation at all.
    glm::vec3 broad_low, broad_high;
    if (get_local_bounds(broad_low, broad_high)) {
        const float model_radius = std::max(glm::length(broad_low), glm::length(broad_high)) * std::abs(m_scale) + 0.75f;
        const glm::vec3 separation = m_position - origin;
        const float reach_radius = max_distance + model_radius;
        if (glm::dot(separation, separation) > reach_radius * reach_radius) return best;
    }
    glm::mat4 model(1.0f);
    model = glm::translate(model, m_position);
    model = glm::rotate(model, glm::radians(m_yaw_degrees), glm::vec3(0.0f, 1.0f, 0.0f));
    if (m_roll_degrees != 0.0f) {
        model = glm::translate(model, glm::vec3(0.0f, m_roll_pivot_y, 0.0f));
        model = glm::rotate(model, glm::radians(m_roll_degrees), glm::vec3(0.0f, 0.0f, 1.0f));
        model = glm::translate(model, glm::vec3(0.0f, -m_roll_pivot_y, 0.0f));
    }
    model = glm::scale(model, glm::vec3(m_scale));
    constexpr float margin = 0.10f;
    const auto evaluate = [&](const glm::vec3& low, const glm::vec3& high,
                              const glm::mat4& transform, Mob_Hit_Zone zone) {
        glm::vec3 min_world(1e30f), max_world(-1e30f);
        for (int corner = 0; corner < 8; ++corner) {
            const glm::vec3 pt((corner & 1) ? high.x : low.x,
                               (corner & 2) ? high.y : low.y,
                               (corner & 4) ? high.z : low.z);
            const glm::vec3 p(transform * glm::vec4(pt, 1.0f));
            min_world = glm::min(min_world, p);
            max_world = glm::max(max_world, p);
        }
        const float hit = mob_ray_aabb(origin, dir, min_world - glm::vec3(margin),
                                       max_world + glm::vec3(margin), max_distance);
        if (hit >= 0.0f && (best.distance < 0.0f || hit < best.distance - 1e-4f ||
            (std::abs(hit - best.distance) < 1e-4f &&
             mob_hit_zone_damage_multiplier(zone) > mob_hit_zone_damage_multiplier(best.zone)))) {
            best.distance = hit;
            best.zone = zone;
        }
    };
    const auto& regions = m_model->get_hit_regions();
    if (!regions.empty()) {
        std::vector<glm::mat4> pose;
        compute_bone_matrices(pose);
        for (const auto& region : regions) {
            const size_t i = static_cast<size_t>(region.bone_index);
            if (i >= pose.size()) continue;
            // pose[i] = current_joint_global * inverse_bind; restore joint-global.
            const glm::mat4 joint = model * pose[i] * region.bind_joint_matrix;
            evaluate(region.local_min, region.local_max, joint, region.zone);
        }
        return best;
    }
    // No skin (horse and unrigged future mobs): split true geometry into body levels.
    glm::vec3 low, high;
    if (!get_local_bounds(low, high)) return best;
    const float h = high.y - low.y;
    if (h <= 0.0f) return best;
    glm::vec3 part_low = low, part_high = high;
    part_high.y = low.y + h * 0.33f;
    evaluate(part_low, part_high, model, Mob_Hit_Zone::Leg);
    part_low.y = part_high.y;
    part_high.y = low.y + h * 0.78f;
    evaluate(part_low, part_high, model, Mob_Hit_Zone::Torso);
    part_low.y = part_high.y;
    part_high.y = high.y;
    evaluate(part_low, part_high, model, Mob_Hit_Zone::Head);
    return best;
}

// Simple collision-aware 3D locomotion usable by any new flying mob type.
// Navigation/obstacle planning is separate; this is only the physical step.
void Mob::move_flying(float dt, const glm::vec3& desired_velocity,
                      const std::function<bool(const glm::vec3&)>& collides) {
    if (m_movement_mode != Movement_Mode::Flying || dt <= 0.0f) return;
    dt = std::min(dt, 0.05f);
    const float factor = 1.0f - std::exp(-6.0f * dt);
    m_flight_velocity += (desired_velocity - m_flight_velocity) * factor;
    glm::vec3 candidate = m_position;
    for (int axis = 0; axis < 3; ++axis) {
        candidate[axis] += m_flight_velocity[axis] * dt;
        if (collides(candidate)) {
            candidate[axis] = m_position[axis];
            m_flight_velocity[axis] = 0.0f;
        }
    }
    m_position = candidate;
    const glm::vec2 planar(m_flight_velocity.x, m_flight_velocity.z);
    if (glm::dot(planar, planar) > 0.02f) {
        m_yaw_degrees = glm::degrees(std::atan2(m_flight_velocity.x, m_flight_velocity.z));
    }
}

void Mob::navigate_flying(float dt, const glm::vec3& target, float speed, float width, float height,
                          Mob_Nav::Agent& agent, Mob_Nav::System& nav, const Chunk_Manager& chunks) {
    set_movement_mode(Movement_Mode::Flying);
    const auto steer=agent.steer(nav,chunks,dt,get_position(),target,width,height,Mob_Nav::Travel::Flying,1);
    move_flying(dt,steer.direction*speed,[&](const glm::vec3& feet) {
        return !nav.passable(chunks,feet,width,height,Mob_Nav::Travel::Flying);
    });
    if (glm::length(glm::vec2(steer.direction.x,steer.direction.z))>0.05f)
        set_yaw_degrees(glm::degrees(std::atan2(steer.direction.x,steer.direction.z)));
}
