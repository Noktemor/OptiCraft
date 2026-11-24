#include "mobs/village/Village_System.h"
#include "world/chunks/Chunk_Manager.h"
#include "entities/player/Inventory.h"
#include "world/blocks/Block_Types.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <unordered_set>

namespace {
    using Village::Cell;
    constexpr float k_scan_radius=40.0f;
    constexpr float k_claim_radius=45.0f;
    constexpr float k_active_radius=100.0f;
    std::mt19937& random_engine() {static std::mt19937 engine{0xB177A6Eu};return engine;}
    float random_float(float a,float b) {return std::uniform_real_distribution<float>(a,b)(random_engine());}
    float horizontal_distance(const glm::vec3& a,const glm::vec3& b) {
        return glm::length(glm::vec2(a.x-b.x,a.z-b.z));
    }
    bool loaded(const Chunk_Manager& world,Cell p) {
        const glm::ivec3 c=world.get_chunk_coords_for_world(p.x,p.y,p.z);
        return world.is_chunk_loaded(c.x,c.y,c.z);
    }
    Village::Cell_Data describe(const Chunk_Manager& world,Cell cell) {
        Village::Cell_Data data;
        data.loaded=loaded(world,cell);
        if (!data.loaded) return data;
        const Block_Types b=world.get_block_world(cell.x,cell.y,cell.z);
        const Block_Properties& props=get_block_props(b);
        data.solid=props.is_solid;
        data.door=props.shape==Block_Shape::Door;
        data.bed=props.shape==Block_Shape::Bed;
        data.torch=b==Block_Types::Torch;
        data.hazard=b==Block_Types::Lava||b==Block_Types::Fire||b==Block_Types::Cactus;
        return data;
    }
    bool mature_crop(Block_Types block) {
        return block==Block_Types::Wheat_Crop_3||block==Block_Types::Carrot_Crop_3;
    }
    glm::vec3 neighbor_of(const glm::vec3& pos,float radius) {
        const float angle=random_float(0.0f,6.2831853f);
        return pos+glm::vec3(std::cos(angle)*radius,0.0f,std::sin(angle)*radius);
    }
    std::string model_path(Village::Profession p) {
        switch(p) {
            case Village::Profession::Farmer:return "assets/mobs/villager_farmer.glb";
            case Village::Profession::Archer:return "assets/mobs/villager_archer.glb";
            case Village::Profession::Librarian:return "assets/mobs/villager_librarian.glb";
            case Village::Profession::Hunter:return "assets/mobs/villager_hunter.glb";
            case Village::Profession::Cook:return "assets/mobs/villager_cook.glb";
            case Village::Profession::Smith:return "assets/mobs/villager_smith.glb";
            default:return "assets/mobs/villager_unemployed.glb";
        }
    }
    bool near(const glm::vec3& a,const glm::vec3& b,float distance) {
        return horizontal_distance(a,b)<distance && std::abs(a.y-b.y)<12.0f;
    }
    // The room entrance is a two-block door. Toggle both halves atomically from
    // the gameplay thread; navigation sees the updated passage on its next snapshot.
    void open_door(Chunk_Manager& world,Cell pos) {
        for (int y=pos.y;y<=pos.y+1;++y) {
            const Block_Types block=world.get_block_world(pos.x,y,pos.z);
            if (get_block_props(block).shape!=Block_Shape::Door) continue;
            const uint8_t meta=world.get_block_meta_world(pos.x,y,pos.z);
            if (!(meta&4u)) world.set_block_meta_world(pos.x,y,pos.z,meta|4u);
        }
    }
    void close_door(Chunk_Manager& world,Cell pos) {
        for (int y=pos.y;y<=pos.y+1;++y) {
            const Block_Types block=world.get_block_world(pos.x,y,pos.z);
            if (get_block_props(block).shape!=Block_Shape::Door) continue;
            const uint8_t meta=world.get_block_meta_world(pos.x,y,pos.z);
            if (meta&4u) world.set_block_meta_world(pos.x,y,pos.z,meta&~4u);
        }
    }
}

void Village_System::clear() {
    m_open_trader=0;m_aim_id=0;m_aim_golem=false;
    m_golems.clear();m_residents.clear();m_homes.clear();m_next_id=1;
    m_scan_timer=1.0f;m_breed_timer=12.0f;m_spawn_timer=0.0f;m_golem_timer=2.0f;
    m_last_scan={100000,0,100000};
}

Village_System::Resident* Village_System::spawn_villager(const glm::vec3& pos,Village::Profession p,bool adult) {
    auto v=std::make_unique<Resident>();
    v->id=m_next_id++;v->profession=p;v->age=adult?600.0f:0.0f;
    v->mob.set_position(pos);v->mob.set_anthropomorphic(true);
    v->mob.load(model_path(p));
    if (!adult) v->mob.set_scale(0.55f);
    v->goal=pos;
    Resident* raw=v.get();m_residents.push_back(std::move(v));return raw;
}
Village_System::Golem* Village_System::spawn_golem(const glm::vec3& pos) {
    auto g=std::make_unique<Golem>();
    g->id=m_next_id++;g->mob.set_position(pos);g->mob.load("assets/mobs/iron_golem.glb");
    g->home=pos;g->goal=pos;
    Golem* raw=g.get();m_golems.push_back(std::move(g));return raw;
}

void Village_System::discover_homes(const glm::vec3& player,Chunk_Manager& world) {
    // A limited player-centered scan (no world-wide block queries or chunk generation).
    const int cx=static_cast<int>(std::floor(player.x));
    const int cy=static_cast<int>(std::floor(player.y));
    const int cz=static_cast<int>(std::floor(player.z));
    const auto read=[&](Cell p){return describe(world,p);};
    std::vector<Village::Home> found;
    for (int y=std::max(1,cy-7);y<=cy+7;++y)
      for (int z=cz-40;z<=cz+40;++z)
        for (int x=cx-40;x<=cx+40;++x) {
            Cell bed{x,y,z};
            if (!loaded(world,bed) || get_block_props(world.get_block_world(x,y,z)).shape!=Block_Shape::Bed) continue;
            if (auto home=Village::find_home(bed,read)) found.push_back(*home);
        }
    // Merge; keep known homes until their chunks are available again. Revalidate
    // loaded old houses, so breaking a wall or door revokes the bed claim.
    for (auto it=m_homes.begin();it!=m_homes.end();) {
        if (loaded(world,it->bed) && near(feet(it->bed),player,50.0f) &&
            !Village::find_home(it->bed,read)) it=m_homes.erase(it);
        else ++it;
    }
    for (const Village::Home& home:found) {
        const bool exists=std::any_of(m_homes.begin(),m_homes.end(),[&](const auto& old){return old.bed==home.bed;});
        if (!exists) m_homes.push_back(home);
    }
    // Limit memory for long explorations; retained citizens are persisted separately.
    if (m_homes.size()>1024) m_homes.erase(m_homes.begin(),m_homes.begin()+static_cast<long>(m_homes.size()-1024));
}

void Village_System::assign_homes_and_jobs(const glm::vec3& player,Chunk_Manager& world) {
    std::unordered_set<Cell,Village::Cell_Hash> claimed_beds;
    std::unordered_set<Cell,Village::Cell_Hash> claimed_jobs;
    for (auto& r:m_residents) {
        r->has_home=false;
        if (r->health<=0) continue;
        const auto it=std::find_if(m_homes.begin(),m_homes.end(),[&](const Village::Home& h){return h.bed==r->home;});
        if (it!=m_homes.end() && claimed_beds.insert(r->home).second) r->has_home=true;
    }
    for (auto& r:m_residents) {
        if (!r->alive() || r->has_home) continue;
        const Village::Home* best=nullptr;float distance=k_claim_radius;
        for (const auto& h:m_homes) {
            if (claimed_beds.count(h.bed)) continue;
            const float d=horizontal_distance(r->mob.get_position(),feet(h.inside));
            if (d<distance) {distance=d;best=&h;}
        }
        if (best) {r->home=best->bed;r->has_home=true;claimed_beds.insert(best->bed);}
    }
    // Professions require a unique claimable workstation within walking distance.
    // Scan locally once per discovery cycle (not per mob/tick).
    struct Job { Cell pos;Village::Profession p; };
    std::vector<Job> jobs;
    const int cx=static_cast<int>(std::floor(player.x)),cy=static_cast<int>(std::floor(player.y)),cz=static_cast<int>(std::floor(player.z));
    for (int y=std::max(1,cy-7);y<=cy+7;++y)
      for (int z=cz-40;z<=cz+40;++z)
        for (int x=cx-40;x<=cx+40;++x) {
            Cell at{x,y,z};if (!loaded(world,at)) continue;
            Village::Profession p=Village::job_for_block(static_cast<uint16_t>(world.get_block_world(x,y,z)));
            if (p!=Village::Profession::Unemployed) jobs.push_back({at,p});
        }
    // Avoid relabeling on every tick, but allow changing job after workstation broken.
    for (auto& r:m_residents) {
        if (!r->alive() || !near(r->mob.get_position(),player,55.0f)) continue;
        r->has_job=false;
        for (const Job& job:jobs) {
            if (job.pos==r->job_site && claimed_jobs.insert(job.pos).second) {
                r->has_job=true;r->profession=job.p;break;
            }
        }
        if (r->has_job) continue;
        const Job* best=nullptr;float distance=35.0f;
        for (const Job& job:jobs) {
            if (claimed_jobs.count(job.pos)) continue;
            float d=horizontal_distance(r->mob.get_position(),feet(job.pos));
            if (d<distance) {distance=d;best=&job;}
        }
        if (best) {
            r->has_job=true;r->job_site=best->pos;r->profession=best->p;
            claimed_jobs.insert(best->pos);
        } else r->profession=Village::Profession::Unemployed;
        // Reuse the shared GLB cache, only swap model if profession changed.
        r->mob.load(model_path(r->profession));
        r->mob.set_scale(r->adult()?1.0f:0.55f);
    }
}

void Village_System::walk(Mob& mob,Mob_Nav::Agent& agent,float dt,const glm::vec3& target,
                          float speed,float width,float height,Chunk_Manager& world,Mob_Nav::System& nav) {
    const glm::vec3 pos=mob.get_position();
    if (horizontal_distance(pos,target)<0.30f && std::abs(pos.y-target.y)<1.5f) return;
    Mob_Nav::Steering steer=agent.steer(nav,world,dt,pos,target,width,height,Mob_Nav::Travel::Ground,2);
    glm::vec3 direction=steer.direction;
    if (glm::dot(direction,direction)<0.01f) return;
    glm::vec3 next=pos+glm::vec3(direction.x,0.0f,direction.z)*std::min(dt,0.10f)*speed;
    // Step up one block if A* marked a jump. Do not teleport through the ceiling.
    if (steer.jump && nav.passable(world,next+glm::vec3(0,1,0),width,height,Mob_Nav::Travel::Ground))
        next.y+=1.0f;
    else if (!nav.passable(world,next,width,height,Mob_Nav::Travel::Ground)) {
        // Step down across minor ledges, otherwise stop and let the agent replan.
        if (nav.passable(world,next-glm::vec3(0,1,0),width,height,Mob_Nav::Travel::Ground)) next.y-=1.0f;
        else return;
    }
    // Diagonal movement is checked by nav.passable at the destination; the
    // single sub-step prevents tunnelling through thin blocks.
    mob.set_position(next);
    mob.set_yaw_degrees(glm::degrees(std::atan2(direction.x,direction.z)));
}

void Village_System::consider_birth(const glm::vec3& player,Chunk_Manager& world) {
    if (m_residents.size()>=256) return;
    // Houses provide bed capacity. No vacant bed -> no breeding.
    for (const auto& house:m_homes) {
        if (!near(feet(house.bed),player,35.0f)) continue;
        bool occupied=false;
        for (const auto& r:m_residents) if (r->alive() && r->has_home && r->home==house.bed) {occupied=true;break;}
        if (occupied) continue;
        Resident* parent_a=nullptr,*parent_b=nullptr;
        for (auto& r:m_residents) {
            if (!r->alive() || !r->adult() || r->breeding_cooldown>0 ||
                !near(r->mob.get_position(),feet(house.inside),28.0f)) continue;
            if (!parent_a) parent_a=r.get();
            else if (!parent_b && r.get()!=parent_a) {parent_b=r.get();break;}
        }
        if (!parent_a || !parent_b) continue;
        bool farmer=false,crops=false;
        for (auto& r:m_residents)
            farmer|=r->alive() && r->adult() && r->profession==Village::Profession::Farmer &&
                    near(r->mob.get_position(),feet(house.inside),30.0f);
        if (!farmer) continue;
        const int x=house.bed.x,y=house.bed.y,z=house.bed.z;
        for (int dz=-18;dz<=18 && !crops;dz+=2)
            for (int dx=-18;dx<=18 && !crops;dx+=2) {
                Cell crop{x+dx,y,z+dz};
                if (!loaded(world,crop)) continue;
                for (int dy=-3;dy<=3 && !crops;++dy)
                    crops|=mature_crop(world.get_block_world(crop.x,crop.y+dy,crop.z));
            }
        if (!crops) continue;
        Resident* child=spawn_villager(feet(house.inside),Village::Profession::Unemployed,false);
        child->home=house.bed;child->has_home=true;
        parent_a->breeding_cooldown=120.0f;parent_b->breeding_cooldown=120.0f;
        break; // budget: at most one birth per scan period
    }
}

void Village_System::maintain_golems(const glm::vec3& player,Chunk_Manager& world) {
    if (m_golems.size()>=96) return;
    // Cluster connected homes; villagers count per cluster. No duplication from
    // neighboring houses in the same village. Unloaded houses keep their anchors.
    std::vector<char> visited(m_homes.size(),false);
    for (size_t i=0;i<m_homes.size();++i) {
        if (visited[i] || !near(feet(m_homes[i].bed),player,65.0f)) continue;
        std::vector<size_t> cluster{i};visited[i]=true;
        for (size_t k=0;k<cluster.size();++k)
            for (size_t j=0;j<m_homes.size();++j) {
                if (visited[j]) continue;
                if (near(feet(m_homes[j].bed),feet(m_homes[cluster[k]].bed),48.0f)) {
                    visited[j]=true;cluster.push_back(j);
                }
            }
        glm::vec3 center(0.0f);
        for (size_t h:cluster) center+=feet(m_homes[h].inside);
        center/=static_cast<float>(cluster.size());
        int villagers=0,golems=0;
        for (const auto& r:m_residents) if(r->alive() && near(r->mob.get_position(),center,70.0f))++villagers;
        for (const auto& g:m_golems) if(g->health>0 && near(g->home,center,70.0f))++golems;
        if (golems>=Village::golem_limit(villagers)) continue;
        const Village::Home& at=m_homes[cluster.front()];
        const glm::vec3 entrance=feet(at.door);
        bool placed=false;
        for (int dz=-6;dz<=6 && !placed;dz+=2)
            for (int dx=-6;dx<=6 && !placed;dx+=2) {
                if (dx==0 && dz==0) continue;
                glm::vec3 candidate=entrance+glm::vec3(static_cast<float>(dx),0.0f,static_cast<float>(dz));
                if (loaded(world,{static_cast<int>(std::floor(candidate.x)),static_cast<int>(std::floor(candidate.y)),static_cast<int>(std::floor(candidate.z))}) &&
                    get_block_props(world.get_block_world(static_cast<int>(std::floor(candidate.x)),
                        static_cast<int>(std::floor(candidate.y))-1,static_cast<int>(std::floor(candidate.z)))).is_solid) {
                    bool clear=true;
                    for(int dy=0;dy<3;++dy) {
                        const int bx=static_cast<int>(std::floor(candidate.x));
                        const int by=static_cast<int>(std::floor(candidate.y))+dy;
                        const int bz=static_cast<int>(std::floor(candidate.z));
                        if(get_block_props(world.get_block_world(bx,by,bz)).is_solid)clear=false;
                    }
                    if(clear) {spawn_golem(candidate)->home=center;placed=true;}
                }
            }
        break; // at most one golem added per maintenance pass
    }
}

void Village_System::update_residents(float dt,bool night,Chunk_Manager& world,Mob_Nav::System& nav,
                                      const std::vector<glm::vec3>& enemies) {
    for (auto& r:m_residents) {
        if (!r->alive()) continue;
        const glm::vec3 pos=r->mob.get_position();
        r->age+=dt;r->breeding_cooldown=std::max(0.0f,r->breeding_cooldown-dt);
        r->damage_cooldown=std::max(0.0f,r->damage_cooldown-dt);
        r->panic_timer=std::max(0.0f,r->panic_timer-dt);
        if (r->age>=180.0f && r->mob.get_scale()<0.99f)r->mob.set_scale(1.0f);
        if (!loaded(world,{static_cast<int>(std::floor(pos.x)),static_cast<int>(std::floor(pos.y)),static_cast<int>(std::floor(pos.z))})) continue;
        for (const auto& danger:enemies)
            if (near(pos,danger,5.0f)) {r->danger=danger;r->panic_timer=5.0f;break;}
        r->wander_timer-=dt;
        const auto it=std::find_if(m_homes.begin(),m_homes.end(),[&](const Village::Home& h){return r->has_home && h.bed==r->home;});
        if (r->panic_timer>0) {
            glm::vec3 away=pos-r->danger;away.y=0.0f;
            if (glm::length(away)<0.01f) away={1,0,0};
            r->goal=pos+glm::normalize(away)*11.0f;
            r->wander_timer=0.6f;
        } else if (night && it!=m_homes.end()) {
            r->goal=feet(it->inside);r->wander_timer=0.3f;
            // A closed door blocks A* itself: let the resident claim/open its entrance
            // before submitting the route (other species cannot interact with doors).
            if (near(pos,feet(it->door),12.0f))open_door(world,it->door);
        } else if (r->wander_timer<=0.0f) {
            if (r->has_job && r->adult()) {
                r->goal=neighbor_of(feet(r->job_site),1.5f);
                r->wander_timer=random_float(4.0f,8.0f);
            } else if(it!=m_homes.end()) {
                r->goal=neighbor_of(feet(it->door),random_float(3.0f,9.0f));
                r->wander_timer=random_float(4.0f,8.0f);
            } else {
                r->goal=neighbor_of(pos,random_float(3.0f,7.0f));
                r->wander_timer=random_float(5.0f,9.0f);
            }
        }
        if (!night && it!=m_homes.end() && near(pos,feet(it->door),3.0f))open_door(world,it->door);
        walk(r->mob,r->navigation,dt,r->goal,r->panic_timer>0?3.5f:1.7f,0.58f,1.88f,world,nav);
        r->mob.update(dt);
    }
}

int Village_System::update_golems(float dt,const glm::vec3& player,Chunk_Manager& world,Mob_Nav::System& nav,
                                  const std::vector<glm::vec3>& enemies,
                                  const std::function<bool(const glm::vec3&,float,float)>& damage_enemy) {
    int player_damage=0;
    for(auto& g:m_golems) {
        if (g->health<=0)continue;
        const glm::vec3 pos=g->mob.get_position();
        if (!loaded(world,{static_cast<int>(std::floor(pos.x)),static_cast<int>(std::floor(pos.y)),static_cast<int>(std::floor(pos.z))}))continue;
        g->attack_timer=std::max(0.0f,g->attack_timer-dt);
        g->player_aggro_timer=std::max(0.0f,g->player_aggro_timer-dt);
        glm::vec3 target{};bool aggro=false;
        float best=16.0f;
        for(const auto& hostile:enemies) {
            if (!near(hostile,g->home,35.0f)) continue;
            const float d=horizontal_distance(hostile,pos);
            if(d<best){best=d;target=hostile;aggro=true;}
        }
        if(g->player_aggro_timer>0 && near(player,pos,18.0f)){
            aggro=true;target=player;best=horizontal_distance(player,pos);
        }
        if(aggro){
            g->goal=target;
            if(best<2.6f && g->attack_timer<=0.0f){
                if(g->player_aggro_timer>0 && std::abs(player.y-pos.y)<3.5f) {
                    g->player_hostile=true;
                    player_damage+=12;
                } else damage_enemy(pos,3.5f,12.0f);
                g->attack_timer=1.3f;
            }
        } else {
            g->wander_timer-=dt;
            if(g->wander_timer<=0) {
                g->goal=neighbor_of(g->home,random_float(3.0f,10.0f));
                g->wander_timer=random_float(5.0f,11.0f);
            }
        }
        walk(g->mob,g->navigation,dt,g->goal,aggro?3.2f:1.3f,1.2f,2.75f,world,nav);
        g->mob.update(dt);
    }
    return player_damage;
}

Village_System::Tick_Result Village_System::tick(float dt,const glm::vec3& player,bool night,Chunk_Manager& world,
                          Mob_Nav::System& nav,const std::vector<glm::vec3>& enemies,
                          const std::function<bool(const glm::vec3&,float,float)>& damage_enemy) {
    if(dt<=0.0f)return {};
    dt=std::min(dt,0.10f);m_scan_timer-=dt;m_breed_timer-=dt;m_golem_timer-=dt;
    m_attack_cooldown=std::max(0.0f,m_attack_cooldown-dt);
    if(m_scan_timer<=0.0f && (horizontal_distance(player,m_last_scan)>6.0f || m_scan_timer<=-8.0f)) {
        std::erase_if(m_residents,[](const auto& r){return !r->alive();});
        discover_homes(player,world);
        assign_homes_and_jobs(player,world);
        m_last_scan=player;m_scan_timer=10.0f;
        // Generated villages begin with two residents, later births need food/farmer.
        int near_count=0;
        for(const auto& r:m_residents) if(r->alive() && near(r->mob.get_position(),player,45.0f))++near_count;
        if(near_count<2) for(const auto& h:m_homes) {
            if(near_count>=2 || m_residents.size()>=256) break;
            if(!near(feet(h.bed),player,38.0f)) continue;
            bool occupied=false;
            for(const auto& r:m_residents)if(r->alive()&&r->has_home&&r->home==h.bed)occupied=true;
            if(occupied)continue;
            auto* v=spawn_villager(feet(h.inside),Village::Profession::Unemployed,true);
            v->home=h.bed;v->has_home=true;++near_count;
        }
        assign_homes_and_jobs(player,world);
    }
    if(m_breed_timer<=0.0f){consider_birth(player,world);m_breed_timer=12.0f;}
    if(m_golem_timer<=0.0f){
        std::erase_if(m_golems,[](const auto& g){return g->health<=0.0f;});
        maintain_golems(player,world);
        m_golem_timer=5.0f;
    }
    update_residents(dt,night,world,nav,enemies);
    const int golem_damage=update_golems(dt,player,world,nav,enemies,damage_enemy);
    // Close doors once the house is occupied at night. A resident approaching its
    // entrance reopens it; daytime doors are left under player/AI control.
    if(night) for(const auto& h:m_homes) {
        bool approaching=false,home=false;
        for(const auto& r:m_residents) if(r->alive() && r->has_home && r->home==h.bed) {
            home=true;
            if(horizontal_distance(r->mob.get_position(),feet(h.inside))>0.85f) approaching=true;
        }
        if(home && !approaching)close_door(world,h.door);
    }
    return {golem_damage};
}

void Village_System::render(const Shader& shader,const glm::mat4& view,const glm::mat4& projection,float ambient) const {
    for(const auto& v:m_residents)if(v->alive())v->mob.render(shader,view,projection,ambient);
    for(const auto& g:m_golems)if(g->health>0)g->mob.render(shader,view,projection,ambient);
}
float Village_System::aim(const glm::vec3& eye,const glm::vec3& direction,float reach) {
    m_aim_id=0;m_aim_golem=false;float best=-1.0f;
    for(const auto& r:m_residents) {
        if(!r->alive())continue;
        auto hit=r->mob.raycast_body_part(eye,direction,reach);
        if(hit && (best<0 || hit.distance<best)){best=hit.distance;m_aim_id=r->id;m_aim_golem=false;}
    }
    for(const auto& g:m_golems) {
        if(g->health<=0)continue;
        auto hit=g->mob.raycast_body_part(eye,direction,reach);
        if(hit && (best<0 || hit.distance<best)){best=hit.distance;m_aim_id=g->id;m_aim_golem=true;}
    }
    return best;
}
const Village_System::Resident* Village_System::trader() const {
    for(const auto& r:m_residents)if(r->id==m_open_trader && r->alive())return r.get();
    return nullptr;
}
bool Village_System::interact(bool right,bool left,const glm::vec3& player,const Item_Stack& held,
                              bool survival,std::string& feedback) {
    if(m_aim_id==0)return false;
    if(right) {
        if(m_aim_golem)return false;
        for(const auto& r:m_residents) if(r->id==m_aim_id && r->alive()){
            if(!r->adult()){feedback="Villager is too young to trade";return true;}
            if(r->profession==Village::Profession::Unemployed){feedback="Villager needs a workstation";return true;}
            m_open_trader=r->id;return true;
        }
    }
    if(left && m_attack_cooldown<=0.0f) {
        float damage=survival?std::max(1.0f,get_block_props(held.type).attack_damage):1000.0f;
        m_attack_cooldown=0.55f;
        if(m_aim_golem) {
            for(auto& g:m_golems)if(g->id==m_aim_id && g->health>0){
                g->health-=damage;g->player_aggro_timer=20.0f;g->player_hostile=true;
                g->mob.set_hurt_flash(0.8f);
                return true;
            }
        } else {
            for(auto& r:m_residents)if(r->id==m_aim_id && r->alive()) {
                r->health-=damage;r->mob.set_hurt_flash(0.8f);
                alert_at(r->mob.get_position(),player,r->health<=0);
                for(auto& g:m_golems)if(near(g->home,r->mob.get_position(),38.0f)){
                    g->player_hostile=true;g->player_aggro_timer=25.0f;
                }
                if(r->health<=0 && m_open_trader==r->id)m_open_trader=0;
                return true;
            }
        }
    }
    return false;
}
void Village_System::alert_at(const glm::vec3& location,const glm::vec3& attacker,bool death) {
    for(auto& r:m_residents)if(r->alive() && near(r->mob.get_position(),location,death?22.0f:14.0f)){
        r->panic_timer=death?12.0f:6.0f;r->danger=attacker;
    }
}
bool Village_System::trade(int index,Inventory& inventory,std::string& feedback) {
    const Resident* resident=trader();
    if(!resident || index<0 || index>2)return false;
    const auto offer=Village::offers(resident->profession)[static_cast<size_t>(index)];
    if(offer.cost==0 || offer.result==0)return false;
    const Block_Types cost=static_cast<Block_Types>(offer.cost),item=static_cast<Block_Types>(offer.result);
    // Transactional: try on an inventory copy, commit only if the entire output
    // fits. No partial consumption or currency duplication on full inventory.
    Inventory trial=inventory;
    if(!trial.has_resource(cost,offer.cost_count) || !trial.remove_resource(cost,offer.cost_count)) {
        feedback="Not enough "+get_block_props(cost).name;return false;
    }
    if(trial.try_add(item,offer.result_count)!=offer.result_count) {
        feedback="Inventory is full";return false;
    }
    inventory=std::move(trial);
    feedback="Traded "+std::to_string(offer.cost_count)+" "+get_block_props(cost).name+
             " for "+std::to_string(offer.result_count)+" "+get_block_props(item).name;
    return true;
}
std::string Village_System::serialize() const {
    std::ostringstream out;
    out<<"VILLAGE_1 "<<m_next_id<<'\n'<<std::setprecision(std::numeric_limits<float>::max_digits10);
    for(const auto& r:m_residents)if(r->alive()) {
        const glm::vec3 p=r->mob.get_position();
        out<<"R "<<r->id<<' '<<p.x<<' '<<p.y<<' '<<p.z<<' '<<static_cast<int>(r->profession)<<' '
           <<r->home.x<<' '<<r->home.y<<' '<<r->home.z<<' '<<r->has_home<<' '
           <<r->job_site.x<<' '<<r->job_site.y<<' '<<r->job_site.z<<' '<<r->has_job<<' '
           <<r->age<<' '<<r->health<<' '<<r->breeding_cooldown<<'\n';
    }
    for(const auto& g:m_golems)if(g->health>0){
        const glm::vec3 p=g->mob.get_position();
        out<<"G "<<g->id<<' '<<p.x<<' '<<p.y<<' '<<p.z<<' '
           <<g->home.x<<' '<<g->home.y<<' '<<g->home.z<<' '<<g->health<<'\n';
    }
    return out.str();
}
void Village_System::deserialize(const std::string& data) {
    clear();std::istringstream in(data);std::string header;uint64_t next=1;
    if(!(in>>header>>next) || header!="VILLAGE_1")return;
    std::string type;
    while(in>>type) {
        if(type=="R"){
            uint64_t id;glm::vec3 pos;int profession,has_home,has_job;
            Cell home,job;float age,hp,cooldown;
            if(!(in>>id>>pos.x>>pos.y>>pos.z>>profession>>home.x>>home.y>>home.z>>has_home>>
                 job.x>>job.y>>job.z>>has_job>>age>>hp>>cooldown))break;
            if(!std::isfinite(pos.x)||!std::isfinite(pos.y)||!std::isfinite(pos.z)||hp<=0 || m_residents.size()>=256)continue;
            auto* v=spawn_villager(pos,static_cast<Village::Profession>(std::clamp(profession,0,6)),age>=180.0f);
            v->id=id;v->home=home;v->job_site=job;v->has_home=has_home!=0;v->has_job=has_job!=0;
            v->age=age;v->health=std::min(hp,20.0f);v->breeding_cooldown=std::max(0.0f,cooldown);
            m_next_id=std::max(m_next_id,id+1);
        } else if(type=="G"){
            uint64_t id;glm::vec3 pos,home;float hp;
            if(!(in>>id>>pos.x>>pos.y>>pos.z>>home.x>>home.y>>home.z>>hp))break;
            if(!std::isfinite(pos.x)||hp<=0 || m_golems.size()>=96)continue;
            auto* g=spawn_golem(pos);g->id=id;g->home=home;g->health=std::min(hp,100.0f);
            m_next_id=std::max(m_next_id,id+1);
        } else break;
    }
    m_next_id=std::max(m_next_id,next);
}
