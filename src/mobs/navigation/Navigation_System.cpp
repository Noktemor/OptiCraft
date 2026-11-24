#include "mobs/navigation/Navigation_System.h"
#include "world/chunks/Chunk_Manager.h"
#include "world/blocks/Block_Types.h"
#include "utils/configs/Config_World.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Mob_Nav {
namespace {
    Cell cell_of(const glm::vec3& p) {
        return {static_cast<int>(std::floor(p.x)),static_cast<int>(std::floor(p.y+0.13f)),
                static_cast<int>(std::floor(p.z))};
    }
    glm::vec3 feet_of(Cell c) { return {c.x+0.5f,static_cast<float>(c.y),c.z+0.5f}; }
    float horizontal_dist2(const glm::vec3& a, const glm::vec3& b) {
        const float dx=a.x-b.x, dz=a.z-b.z;
        return dx*dx+dz*dz;
    }
}

System::System() : m_worker(&System::run, this) {}
System::~System() {
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
        m_jobs.clear();
    }
    m_cv.notify_one();
    if (m_worker.joinable()) m_worker.join();
}

void System::clear() {
    m_epoch.fetch_add(1, std::memory_order_release);
    m_pending.clear();
    std::lock_guard lock(m_mutex);
    m_jobs.clear();
}

uint64_t System::submit(std::weak_ptr<Reply> reply, const glm::vec3& from, const glm::vec3& to,
                        Search_Settings settings, int priority) {
    const uint64_t id = m_next_ticket++;
    // Bounded producer queue, prioritize fights and panic over idle wandering.
    if (m_pending.size() >= 192) {
        const auto it = std::max_element(m_pending.begin(),m_pending.end(),
                                         [](const Request& a,const Request& b) {return a.priority < b.priority;});
        if (it != m_pending.end() && it->priority >= priority) m_pending.erase(it);
        else return 0;
    }
    m_pending.push_back({std::move(reply),from,to,settings,id,m_epoch.load(),priority});
    return id;
}

System::Job System::make_snapshot(Request request, const Chunk_Manager& world) {
    Job result;
    result.request = std::move(request);
    result.from = cell_of(result.request.from);
    const glm::vec3 displacement = result.request.to-result.request.from;
    const float dist = glm::length(glm::vec2(displacement.x,displacement.z));
    // Bounded local A*: distant goals advance through successive 18-block corridors.
    const float scale = dist > 18.0f ? 18.0f/dist : 1.0f;
    glm::vec3 local_goal = result.request.from+displacement*scale;
    if (result.request.settings.travel == Travel::Ground)
        local_goal.y = std::clamp(local_goal.y,result.request.from.y-4.0f,result.request.from.y+4.0f);
    result.goal = cell_of(local_goal);
    constexpr int margin_xz = 8;
    constexpr int margin_y = 5;
    Grid& g = result.snapshot;
    g.ox = std::min(result.from.x,result.goal.x)-margin_xz;
    g.oz = std::min(result.from.z,result.goal.z)-margin_xz;
    g.oy = std::max(Config::world_min_y,std::min(result.from.y,result.goal.y)-margin_y);
    const int end_x = std::max(result.from.x,result.goal.x)+margin_xz;
    const int end_z = std::max(result.from.z,result.goal.z)+margin_xz;
    const int end_y = std::min(Config::world_max_y,std::max(result.from.y,result.goal.y)+margin_y+
                               static_cast<int>(std::ceil(result.request.settings.height))+1);
    g.sx = end_x-g.ox+1;
    g.sz = end_z-g.oz+1;
    g.sy = end_y-g.oy+1;
    g.voxels.resize(static_cast<size_t>(g.sx)*g.sy*g.sz);
    for (int y=g.oy;y<g.oy+g.sy;++y)
        for (int z=g.oz;z<g.oz+g.sz;++z)
            for (int x=g.ox;x<g.ox+g.sx;++x) {
                const Block_Types b = world.get_block_world(x,y,z);
                const Block_Properties& props = get_block_props(b);
                const bool open_door = props.shape == Block_Shape::Door &&
                    (world.get_block_meta_world(x,y,z) & 4u) != 0;
                uint8_t flags = props.is_solid && !open_door ? 1 : 0;
                if (b == Block_Types::Water) flags |= 2;
                if (b == Block_Types::Lava || b == Block_Types::Fire || b == Block_Types::Cactus) flags |= 4;
                g.voxels[static_cast<size_t>(g.index(x,y,z))] = flags;
            }
    return result;
}

void System::tick(const Chunk_Manager& world, const glm::vec3& focus) {
    m_focus = focus;
    // Drop canceled agents before snapshotting, don't waste expensive get_block_world calls.
    std::erase_if(m_pending,[](const Request& r) {
        auto reply=r.reply.lock();
        if (!reply) return true;
        std::lock_guard lock(reply->mutex);
        return reply->ticket != r.ticket; // cancelled/overwritten by a newer goal
    });
    std::stable_sort(m_pending.begin(),m_pending.end(),[](const Request& a,const Request& b) {
        return a.priority < b.priority;
    });
    constexpr int max_snapshots_per_tick = 2;
    for (int n=0;n<max_snapshots_per_tick && !m_pending.empty();++n) {
        {
            std::lock_guard lock(m_mutex);
            if (m_jobs.size() >= 6) break; // backpressure: no huge memory/CPU spikes
        }
        Request request = std::move(m_pending.front());
        m_pending.pop_front();
        if (request.reply.expired() || request.epoch != m_epoch.load()) continue;
        Job job = make_snapshot(std::move(request),world);
        {
            std::lock_guard lock(m_mutex);
            m_jobs.emplace_back(std::move(job));
        }
        m_cv.notify_one();
    }
}

void System::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock,[this] { return m_stop || !m_jobs.empty(); });
            if (m_stop) return;
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
        }
        if (job.request.epoch != m_epoch.load(std::memory_order_acquire)) continue;
        auto reply = job.request.reply.lock();
        if (!reply) continue;
        Search_Result result = search(job.snapshot,job.from,job.goal,job.request.settings);
        if (job.request.epoch != m_epoch.load(std::memory_order_acquire)) continue;
        std::lock_guard lock(reply->mutex);
        if (reply->ticket == job.request.ticket) {
            reply->result = std::move(result);
            reply->ready = true;
        }
    }
}

bool System::passable(const Chunk_Manager& world, const glm::vec3& feet, float width, float height,
                      Travel travel) const {
    const float radius = width*0.5f;
    const int x0=static_cast<int>(std::floor(feet.x-radius+0.001f));
    const int x1=static_cast<int>(std::floor(feet.x+radius-0.001f));
    const int z0=static_cast<int>(std::floor(feet.z-radius+0.001f));
    const int z1=static_cast<int>(std::floor(feet.z+radius-0.001f));
    const int y0=static_cast<int>(std::floor(feet.y+0.001f));
    const int y1=static_cast<int>(std::floor(feet.y+height-0.001f));
    if (y0 < Config::world_min_y || y1 > Config::world_max_y) return false;
    for (int y=y0;y<=y1;++y)
        for (int z=z0;z<=z1;++z)
            for (int x=x0;x<=x1;++x) {
                const Block_Types b=world.get_block_world(x,y,z);
                const Block_Properties& props=get_block_props(b);
                const bool open_door=props.shape==Block_Shape::Door &&
                    (world.get_block_meta_world(x,y,z)&4u)!=0;
                if ((props.is_solid && !open_door) || b == Block_Types::Lava || b == Block_Types::Fire || b == Block_Types::Cactus) return false;
            }
    if (travel == Travel::Ground) {
        bool support=false;
        for (int z=z0;z<=z1;++z) for (int x=x0;x<=x1;++x)
            support |= get_block_props(world.get_block_world(x,y0-1,z)).is_solid;
        return support;
    }
    return true;
}

float System::lod_delay(const glm::vec3& from, bool active) const {
    const float d2=horizontal_dist2(from,m_focus);
    if (d2 > 48.0f*48.0f) return active ? 2.8f : 6.0f;
    if (d2 > 28.0f*28.0f) return active ? 1.4f : 3.5f;
    return active ? 0.7f : 2.0f;
}

void Agent::reset() {
    m_route.clear(); m_next=0; m_refresh=0; m_stuck_timer=0; m_request_age=0;
    m_has_progress_anchor=false;
    m_waiting=false; m_ticket=0; m_had_result=false;
    // Late worker replies must not be able to resurrect an old route.
    std::lock_guard lock(m_reply->mutex);
    m_reply->ticket=0; m_reply->ready=false; m_reply->result={};
}

Steering Agent::steer(System& nav, const Chunk_Manager& world, float dt,
                      const glm::vec3& from, const glm::vec3& goal,
                      float width, float height, Travel mode, int priority) {
    Steering out;
    if (glm::length(goal-from) < 0.7f) { reset(); return out; }
    dt=std::clamp(dt,0.0f,0.25f);
    m_refresh-=dt;
    m_request_age+=dt;
    const bool goal_changed=horizontal_dist2(goal,m_goal) > 2.25f*2.25f ||
                            std::abs(goal.y-m_goal.y) > 1.4f;
    if (goal_changed) {
        m_route.clear(); m_next=0; m_refresh=0.0f; m_had_result=false;
        m_goal=goal;
        m_waiting=false;
        std::lock_guard lock(m_reply->mutex);
        m_reply->ticket=0; m_reply->ready=false;
    }
    if (m_waiting) {
        std::lock_guard lock(m_reply->mutex);
        if (m_reply->ready && m_reply->ticket == m_ticket) {
            m_route=std::move(m_reply->result.points);
            m_next=0;
            m_waiting=false;
            m_had_result=true;
            m_reply->ready=false;
            m_request_age=0;
            // Failed searches are not re-submitted every AI tick: avoid permanent
            // CPU load when the target is sealed behind unbreakable blocks.
            if (m_route.empty()) m_refresh=std::max(0.6f,nav.lod_delay(from,priority<=1));
        }
    }
    // Request disappeared (queue saturation / world reload): allow a retry.
    if (m_waiting && m_request_age > 3.5f) { m_waiting=false; m_refresh=0; m_route.clear(); }

    while (m_next < m_route.size()) {
        const glm::vec3 waypoint=feet_of(m_route[m_next]);
        if (horizontal_dist2(from,waypoint) < 0.18f*0.18f &&
            std::abs(waypoint.y-from.y) < 0.85f) ++m_next;
        else break;
    }
    if (m_next < m_route.size()) {
        const glm::vec3 waypoint=feet_of(m_route[m_next]);
        bool valid=nav.passable(world,waypoint,width,height,mode);
        // A route may contain compressed straight segments. Check swept clearance so
        // newly placed blocks halfway along a segment invalidate the path too.
        const glm::vec3 delta=waypoint-from;
        const float len=glm::length(delta);
        if (valid && len>1.0f) {
            const int segments=std::min(24,static_cast<int>(std::ceil(len/0.65f)));
            for (int i=1;i<segments && valid;++i) {
                const glm::vec3 sample=from+delta*(static_cast<float>(i)/segments);
                // During a 1-block jump the straight interpolation crosses the step;
                // the physical jump is allowed to arc above it, so only test level legs.
                if (mode==Travel::Ground && std::abs(delta.y)>0.5f) continue;
                valid=nav.passable(world,sample,width,height,mode);
            }
        }
        if (!valid) { m_route.clear(); m_next=0; m_refresh=0; m_had_result=false; }
    }
    if (m_next >= m_route.size() && !m_route.empty()) {
        m_route.clear(); m_next=0; m_refresh=0; // advance to the next local corridor
    }
    if (!m_waiting && m_route.empty() && m_refresh<=0.0f) {
        const Search_Settings cfg{width*0.5f,height,mode,mode == Travel::Flying ? 12000 : 8500};
        const uint64_t id = nav.submit(m_reply,from,goal,cfg,priority);
        if (id) {
            std::lock_guard lock(m_reply->mutex);
            m_ticket=id;
            m_reply->ticket=id;
            m_reply->ready=false;
            m_waiting=true;
            m_request_age=0.0f;
            m_refresh=nav.lod_delay(from,priority<=1);
        } else m_refresh=0.5f;
    }

    glm::vec3 waypoint=goal;
    if (m_next < m_route.size()) {
        waypoint=feet_of(m_route[m_next]);
        out.has_route=true;
    } else if (m_had_result && !m_waiting) {
        // No path from the current island: don't continuously push against a wall.
        return out;
    }
    glm::vec3 delta=waypoint-from;
    if (mode == Travel::Ground) delta.y=0.0f;
    const float len=glm::length(delta);
    if (len>0.01f) {
        out.direction=delta/len;
        // Approach a corner waypoint gently so a fast mob does not skip the
        // acceptance radius and oscillate around it at full running speed.
        if (out.has_route && len<0.6f) out.direction=out.direction*std::clamp(len*2.0f,0.15f,1.0f);
    }
    if (mode==Travel::Ground && out.has_route)
        out.jump=waypoint.y > from.y + 0.55f && horizontal_dist2(from,waypoint) < 2.25f;

    // Measure progress over an entire second, never per tick: slow animals
    // legitimately travel less than 0.12 blocks per 20 Hz AI update.
    if (!m_has_progress_anchor) {
        m_last_position=from;
        m_has_progress_anchor=true;
        m_stuck_timer=0.0f;
    }
    m_stuck_timer+=dt;
    if (m_stuck_timer >= 1.0f) {
        const float progressed=glm::length(from-m_last_position);
        if (progressed < 0.24f && glm::length(out.direction)>0.1f && !m_waiting) {
            m_route.clear(); m_next=0; m_refresh=0; m_had_result=false;
        }
        m_last_position=from;
        m_stuck_timer=0.0f;
    }
    return out;
}

} // namespace Mob_Nav
