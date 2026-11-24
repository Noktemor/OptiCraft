#pragma once
// Village rules: geometry, job claims, trading and golem cap. No OpenGL dependencies.
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <queue>
#include <unordered_set>
#include <vector>

namespace Village {
struct Cell { int x=0,y=0,z=0; bool operator==(const Cell&) const = default; };
struct Cell_Hash { size_t operator()(Cell c) const noexcept {
    uint64_t v=(static_cast<uint64_t>(static_cast<uint32_t>(c.x))*0x9E3779B97F4A7C15ULL) ^
               (static_cast<uint64_t>(static_cast<uint32_t>(c.y))*0xBF58476D1CE4E5B9ULL) ^
               (static_cast<uint64_t>(static_cast<uint32_t>(c.z))*0x94D049BB133111EBULL);
    v^=v>>30;v*=0xBF58476D1CE4E5B9ULL;v^=v>>27;return static_cast<size_t>(v);
} };
struct Cell_Data { bool loaded=false,solid=false,door=false,bed=false,torch=false,hazard=false; };
using Read_Cell=std::function<Cell_Data(Cell)>;
struct Home { Cell bed,door,inside; int floor_area=0; bool operator==(const Home&) const = default; };
inline int golem_limit(int residents) {
    // 5 / 12 / 22 / 35 / 51 / 70 / 92 villagers; never more than 7.
    int threshold=5, step=7, count=0;
    while (count < 7 && residents >= threshold) {++count; threshold+=step;step+=3;}
    return count;
}

// A house is a genuinely enclosed accessible room, not just a bed next to a door.
// Flood fill the accessible floor at bed height. Windows and walls count as opaque
// boundaries, door panels as a boundary with entrance. Every interior cell must
// have a roof no more than 5 blocks above floor. A torch must illuminate the room.
inline std::optional<Home> find_home(Cell bed, const Read_Cell& read) {
    const Cell_Data origin=read(bed);
    if (!origin.loaded || !origin.bed) return std::nullopt;
    constexpr std::array<std::array<int,2>,4> directions{{{{1,0}},{{-1,0}},{{0,1}},{{0,-1}}}};
    auto in_radius=[&](Cell c) {return c.y==bed.y &&
       c.x>=bed.x-11 && c.x<=bed.x+11 && c.z>=bed.z-11 && c.z<=bed.z+11;};
    auto inside=[&](Cell c) {
        const Cell_Data b=read(c),head=read({c.x,c.y+1,c.z});
        return b.loaded && head.loaded && !b.solid && !b.bed && !b.door && !b.hazard &&
               !head.solid && !head.door && !head.hazard;
    };
    std::unordered_set<Cell,Cell_Hash> visited;
    std::queue<Cell> pending;
    for (auto [dx,dz]:directions) {
        Cell c{bed.x+dx,bed.y,bed.z+dz};
        if (inside(c) && visited.insert(c).second) pending.push(c);
    }
    if (pending.empty()) return std::nullopt;
    bool torch=false,door=false,leak=false;
    Cell door_at{},entry{};
    constexpr int max_cells=220;
    while (!pending.empty() && !leak) {
        const Cell c=pending.front();pending.pop();
        if (!in_radius(c) || static_cast<int>(visited.size())>max_cells) {leak=true;break;}
        // Reject open sky and rooms with holes in their roofs.
        bool roof=false;
        for (int dy=2;dy<=5;++dy) {
            const Cell_Data above=read({c.x,c.y+dy,c.z});
            if (!above.loaded) {leak=true;break;}
            if (above.solid) {roof=true;break;}
        }
        if (leak || !roof) {leak=true;break;}
        torch |= read(c).torch || read({c.x,c.y+1,c.z}).torch;
        for (auto [dx,dz]:directions) {
            Cell n{c.x+dx,c.y,c.z+dz};
            if (!in_radius(n)) {leak=true;break;}
            Cell_Data b=read(n);
            torch |= b.torch || read({n.x,n.y+1,n.z}).torch;
            if (b.door || read({n.x,n.y+1,n.z}).door) {
                door=true;door_at=n;entry=c;continue;
            }
            if (inside(n) && visited.insert(n).second) pending.push(n);
        }
    }
    if (leak || !door || !torch || visited.size()<3) return std::nullopt;
    return Home{bed,door_at,entry,static_cast<int>(visited.size())};
}

enum class Profession : uint8_t { Unemployed, Farmer, Archer, Librarian, Hunter, Cook, Smith };
inline const char* name(Profession p) {
    switch(p) {
        case Profession::Farmer:return "Farmer";
        case Profession::Archer:return "Archer";
        case Profession::Librarian:return "Librarian";
        case Profession::Hunter:return "Hunter";
        case Profession::Cook:return "Cook";
        case Profession::Smith:return "Smith";
        default:return "Villager";
    }
}
// Stable IDs defined in Block_Types.h. Map remains intentionally data-only.
inline Profession job_for_block(uint16_t b) {
    switch(b) {
        case 533:return Profession::Farmer;
        case 530:return Profession::Archer;
        case 427:return Profession::Librarian;
        case 531:return Profession::Hunter;
        case 532:return Profession::Cook;
        case 534:return Profession::Smith;
        default:return Profession::Unemployed;
    }
}
struct Offer { uint16_t cost=0;int cost_count=0;uint16_t result=0;int result_count=0; };
inline std::array<Offer,3> offers(Profession p) {
    switch(p) {
        case Profession::Farmer:return {{{71,16,535,1},{72,20,535,1},{535,1,134,4}}};
        case Profession::Archer:return {{{77,24,535,1},{535,2,108,1},{535,3,112,1}}};
        case Profession::Librarian:return {{{507,4,535,1},{535,2,507,1},{503,12,535,1}}};
        case Profession::Hunter:return {{{73,8,535,1},{75,8,535,1},{535,2,54,5}}};
        case Profession::Cook:return {{{53,12,535,1},{73,12,535,1},{535,2,74,5}}};
        case Profession::Smith:return {{{78,6,535,1},{535,4,95,1},{535,3,102,1}}};
        default:return {{{0,0,0,0},{0,0,0,0},{0,0,0,0}}};
    }
}
} // namespace Village
