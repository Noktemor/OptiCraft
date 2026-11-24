#pragma once

#include <glm/glm.hpp>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "mobs/navigation/Navigation_AStar.h"

class Chunk_Manager;

namespace Mob_Nav {

struct Reply {
    std::mutex mutex;
    uint64_t ticket = 0;
    bool ready = false;
    Search_Result result;
};

struct Steering {
    glm::vec3 direction{0.0f};
    bool jump = false;
    bool has_route = false;
};

// Owned by Mobs_Update. Reads Chunk_Manager ONLY on the main thread in tick();
// the worker receives an immutable occupancy snapshot and never calls world code.
class System {
public:
    System();
    ~System();
    System(const System&) = delete;
    System& operator=(const System&) = delete;

    void tick(const Chunk_Manager& world, const glm::vec3& focus);
    void clear(); // dimension change / world reload, invalidates all queued jobs

    uint64_t submit(std::weak_ptr<Reply> reply, const glm::vec3& from, const glm::vec3& to,
                    Search_Settings settings, int priority);
    bool passable(const Chunk_Manager& world, const glm::vec3& feet, float width, float height,
                  Travel travel) const;
    float lod_delay(const glm::vec3& from, bool active) const;

private:
    struct Request {
        std::weak_ptr<Reply> reply;
        glm::vec3 from{0.0f}, to{0.0f};
        Search_Settings settings;
        uint64_t ticket = 0, epoch = 0;
        int priority = 0;
    };
    struct Job { Request request; Grid snapshot; Cell from; Cell goal; };

    std::deque<Request> m_pending; // only accessed on main thread
    std::deque<Job> m_jobs;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_worker;
    bool m_stop = false;
    std::atomic<uint64_t> m_epoch{1};
    uint64_t m_next_ticket = 1;
    glm::vec3 m_focus{0.0f};

    static Job make_snapshot(Request request, const Chunk_Manager& world);
    void run();
};

// Small per-mob route follower. Only coordinates + weak result ticket; no own threads.
// On terrain edits it revalidates the upcoming waypoint and requests a fresh route.
class Agent {
public:
    void reset();
    Steering steer(System& nav, const Chunk_Manager& world, float dt,
                   const glm::vec3& from, const glm::vec3& goal,
                   float width, float height, Travel mode, int priority = 2);
private:
    std::shared_ptr<Reply> m_reply = std::make_shared<Reply>();
    std::vector<Cell> m_route;
    size_t m_next = 0;
    glm::vec3 m_goal{0.0f};
    glm::vec3 m_last_position{0.0f};
    float m_refresh = 0.0f;
    float m_stuck_timer = 0.0f;
    bool m_has_progress_anchor = false;
    float m_request_age = 0.0f;
    uint64_t m_ticket = 0;
    bool m_waiting = false;
    bool m_had_result = false;
};

} // namespace Mob_Nav
