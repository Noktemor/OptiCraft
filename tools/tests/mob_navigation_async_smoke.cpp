// Tests the async scheduler against a fake Chunk_Manager that ASSERTS on background reads.
// Build: g++ -std=c++20 -pthread -Isrc -Itools/tests/mob_nav_mock
//         tools/tests/mob_navigation_async_smoke.cpp src/mobs/navigation/Navigation_System.cpp -o /tmp/nav_test
#include "mobs/navigation/Navigation_System.h"
#include "world/chunks/Chunk_Manager.h"
#include <cassert>
#include <chrono>
#include <thread>
#include <iostream>
int main() {
    Chunk_Manager chunks;
    // Three-block-high wall; corridor opening is at z=5.
    for(int z=-4;z<=4;++z)for(int y=0;y<3;++y)chunks.walls.insert({8,y,z});
    Mob_Nav::System system;
    Mob_Nav::Agent agent;
    glm::vec3 feet{1.5f,0.0f,0.5f}, goal{14.5f,0.0f,0.5f};
    bool followed=false, got_route=false;
    for(int i=0;i<3000;++i) {
        auto steering=agent.steer(system,chunks,0.02f,feet,goal,0.6f,1.8f,Mob_Nav::Travel::Ground,0);
        system.tick(chunks,{2.0f,0,0});
        if (steering.has_route) {
            got_route=true;
            if (!system.passable(chunks,feet,0.6f,1.8f,Mob_Nav::Travel::Ground)) {
              std::cerr<<"INVALID AT "<<feet.x<<","<<feet.y<<","<<feet.z<<" step "<<i<<" dir "<<steering.direction.x<<","<<steering.direction.y<<","<<steering.direction.z<<"\n";
              assert(false);
            }
            feet=feet+steering.direction*0.06f;
            if(feet.x>10.0f && feet.z>4.0f)followed=true;
            if(glm::length(feet-goal)<1.0f)break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    assert(got_route);
    assert(followed);
    // Block the old corridor then force a new route. The worker must not touch live chunks.
    agent.reset();
    for(int y=0;y<3;++y)chunks.walls.insert({8,y,5});
    agent.steer(system,chunks,0.05f,{1.5f,0.0f,0.5f},goal,0.6f,1.8f,Mob_Nav::Travel::Ground,0);
    system.tick(chunks,{2,0,0});
    system.clear(); // simulated teleport / dimension switch while work may be running
    agent.reset();

    // Terrain edit halfway along a cached straight route triggers re-pathing.
    Chunk_Manager edited;
    Mob_Nav::System edited_system;
    Mob_Nav::Agent edited_agent;
    glm::vec3 edited_feet{1.5f,0.0f,0.5f},edited_goal{10.5f,0.0f,0.5f};
    bool initially_planned=false, detoured=false;
    for (int i=0;i<200;++i) {
        auto s=edited_agent.steer(edited_system,edited,0.02f,edited_feet,edited_goal,
                                  0.6f,1.8f,Mob_Nav::Travel::Ground,0);
        edited_system.tick(edited,{2,0,0});
        if (s.has_route) { initially_planned=true;break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(initially_planned);
    for(int y=0;y<=2;++y)edited.walls.insert({4,y,0});
    for(int i=0;i<1500;++i) {
        auto s=edited_agent.steer(edited_system,edited,0.02f,edited_feet,edited_goal,
                                  0.6f,1.8f,Mob_Nav::Travel::Ground,0);
        edited_system.tick(edited,{2,0,0});
        if (s.has_route) {
            edited_feet=edited_feet+s.direction*0.055f;
            assert(edited_system.passable(edited,edited_feet,0.6f,1.8f,Mob_Nav::Travel::Ground));
            if (std::abs(edited_feet.z-0.5f)>0.6f) detoured=true;
            if(glm::length(edited_feet-edited_goal)<0.75f) break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    assert(detoured);
    assert(glm::length(edited_feet-edited_goal)<0.75f);
    std::cout<<"mob navigation async smoke: PASS (worker safety, routes, edit/reload invalidation)\n";
}
