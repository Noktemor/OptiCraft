#pragma once
// Village life simulation. Owned by Game, ticked with the existing mob navigation
// worker; all Chunk_Manager reads/writes happen on the gameplay thread.
#include "mobs/village/Village_Rules.h"
#include "mobs/Mob.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <memory>

class Chunk_Manager;
class Shader;
class Inventory;
namespace Mob_Nav { class System; }

class Village_System {
public:
    struct Resident {
        uint64_t id=0;
        Mob mob;
        Village::Profession profession=Village::Profession::Unemployed;
        Village::Cell home{};
        Village::Cell job_site{};
        bool has_home=false,has_job=false;
        float age=600.0f,health=20.0f,breeding_cooldown=0.0f;
        float wander_timer=0.0f,panic_timer=0.0f,damage_cooldown=0.0f;
        glm::vec3 goal{0.0f},danger{0.0f};
        Mob_Nav::Agent navigation;
        bool adult() const {return age>=180.0f;}
        bool alive() const {return health>0.0f;}
    };
    struct Golem {
        uint64_t id=0;
        Mob mob;
        glm::vec3 home{0.0f};
        float health=100.0f,attack_timer=0.0f,damage_cooldown=0.0f;
        float wander_timer=0.0f;
        glm::vec3 goal{0.0f};
        Mob_Nav::Agent navigation;
        bool player_hostile=false;
        float player_aggro_timer=0.0f;
    };
    struct Tick_Result { int player_damage=0; };
    void clear();
    Tick_Result tick(float dt,const glm::vec3& player,bool night,Chunk_Manager& world,
              Mob_Nav::System& nav,const std::vector<glm::vec3>& enemies,
              const std::function<bool(const glm::vec3&,float,float)>& damage_enemy);
    void render(const Shader& shader,const glm::mat4& view,const glm::mat4& projection,float ambient) const;
    float aim(const glm::vec3& eye,const glm::vec3& direction,float reach);
    // On right click, opens a stable resident by ID. Left click causes damage and alarm.
    bool interact(bool right,bool left,const glm::vec3& player,const Item_Stack& held,
                  bool survival,std::string& feedback);
    bool trade(int offer_index,Inventory& inventory,std::string& feedback);
    bool trade_open() const {return m_open_trader!=0 && trader()!=nullptr;}
    void close_trade() {m_open_trader=0;}
    const Resident* trader() const;
    const std::vector<Village::Home>& houses() const {return m_homes;}
    size_t resident_count() const {return m_residents.size();}
    size_t golem_count() const {return m_golems.size();}
    std::string serialize() const;
    void deserialize(const std::string& data);
    // Call when a known hostile hits a villager or a golem: nearby residents panic.
    void alert_at(const glm::vec3& location,const glm::vec3& attacker,bool death);
private:
    std::vector<std::unique_ptr<Resident>> m_residents;
    std::vector<std::unique_ptr<Golem>> m_golems;
    std::vector<Village::Home> m_homes;
    uint64_t m_next_id=1,m_open_trader=0,m_aim_id=0;
    bool m_aim_golem=false;
    float m_scan_timer=1.0f,m_breed_timer=12.0f,m_spawn_timer=0.0f,m_golem_timer=2.0f;
    float m_attack_cooldown=0.0f;
    glm::vec3 m_last_scan{100000,0,100000};
    void discover_homes(const glm::vec3& player,Chunk_Manager& world);
    void assign_homes_and_jobs(const glm::vec3& player,Chunk_Manager& world);
    void consider_birth(const glm::vec3& player,Chunk_Manager& world);
    void maintain_golems(const glm::vec3& player,Chunk_Manager& world);
    void update_residents(float dt,bool night,Chunk_Manager& world,Mob_Nav::System& nav,
                          const std::vector<glm::vec3>& enemies);
    int update_golems(float dt,const glm::vec3& player,Chunk_Manager& world,Mob_Nav::System& nav,
                       const std::vector<glm::vec3>& enemies,
                       const std::function<bool(const glm::vec3&,float,float)>& damage_enemy);
    Resident* spawn_villager(const glm::vec3& feet,Village::Profession profession,bool adult);
    Golem* spawn_golem(const glm::vec3& feet);
    static glm::vec3 feet(Village::Cell c) {return {c.x+0.5f,static_cast<float>(c.y),c.z+0.5f};}
    static void walk(Mob& mob,Mob_Nav::Agent& agent,float dt,const glm::vec3& target,
                     float speed,float width,float height,Chunk_Manager& world,Mob_Nav::System& nav);
};
