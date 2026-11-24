#pragma once

// Small, budgeted fire simulation. Blocks remain authoritative in chunks;
// this list stores only active fires and their remaining lifetime.
#include "world/chunks/Chunk_Manager.h"
#include "utils/configs/Config_World.h"
#include "math/Math_Helpers.h"
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

class Fire_Simulation {
public:
    struct Flame { glm::ivec3 pos; int seconds = 12; };

    static bool flammable(Block_Types type) {
        const auto& props = get_block_props(type);
        if (props.name.empty()) return false;
        std::string n = props.name;
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        // Avoid igniting arbitrary solids (including portal frames and stone).
        for (const char* tag : {"planks", "log", "leaves", "wood", "wool", "bookshelf",
                                 "carpet", "hay bale", "fence", "stairs", "slab", "sapling"})
            if (n.find(tag) != std::string::npos) return true;
        return false;
    }

    bool ignite(const glm::ivec3& at, Chunk_Manager& chunks) {
        if (at.y < Config::world_min_y || at.y > Config::world_max_y ||
            !loaded(at, chunks) || chunks.get_block_world(at.x, at.y, at.z) != Block_Types::Air)
            return false;
        const glm::ivec3 below = at + glm::ivec3(0, -1, 0);
        if (!loaded(below, chunks)) return false;
        const auto base = chunks.get_block_world(below.x, below.y, below.z);
        bool supported = get_block_props(base).is_solid;
        for (const auto& d : kDirections) {
            const auto n = at + d;
            if (loaded(n, chunks) && flammable(chunks.get_block_world(n.x, n.y, n.z))) supported = true;
        }
        if (!supported || m_fires.size() >= kMaxActive || !chunks.set_block_world(at.x, at.y, at.z, Block_Types::Fire))
            return false;
        track(at, 10 + static_cast<int>(hash(at, m_step) % 9));
        return true;
    }

    void tick(float dt, Chunk_Manager& chunks) {
        m_accumulator += std::clamp(dt, 0.0f, 0.25f);
        if (m_accumulator < 1.0f) return;
        m_accumulator -= 1.0f;
        ++m_step;
        if (m_fires.empty()) return;
        const size_t count = std::min(kPerSecondBudget, m_fires.size());
        // Rotate work through active fires instead of starving later entries.
        for (size_t n = 0; n < count && !m_fires.empty(); ++n) {
            if (m_cursor >= m_fires.size()) m_cursor = 0;
            Flame& flame = m_fires[m_cursor];
            const glm::ivec3 pos = flame.pos;
            if (!loaded(pos, chunks)) { ++m_cursor; continue; }
            if (chunks.get_block_world(pos.x, pos.y, pos.z) != Block_Types::Fire) {
                m_fires.erase(m_fires.begin() + static_cast<std::ptrdiff_t>(m_cursor));
                continue;
            }
            bool fuel = false;
            for (const auto& d : kDirections) {
                const glm::ivec3 neighbor = pos + d;
                if (loaded(neighbor, chunks) && flammable(chunks.get_block_world(neighbor.x, neighbor.y, neighbor.z))) {
                    fuel = true; break;
                }
            }
            const uint32_t rnd = hash(pos, m_step);
            if (fuel && (rnd % 4u) == 0u && m_fires.size() < kMaxActive) {
                const auto next = pos + kDirections[(rnd >> 4u) % 6u];
                if (loaded(next, chunks)) {
                    const auto neighbor = chunks.get_block_world(next.x, next.y, next.z);
                    if (flammable(neighbor) && chunks.set_block_world(next.x, next.y, next.z, Block_Types::Fire))
                        track(next, 6 + static_cast<int>((rnd >> 9u) % 8u));
                    else if (neighbor == Block_Types::Air) (void)ignite(next, chunks);
                }
            }
            // Ordinary fire burns out; on flammable blocks it may linger slightly longer.
            if (--m_fires[m_cursor].seconds <= 0) {
                chunks.set_block_world(pos.x, pos.y, pos.z, Block_Types::Air);
                m_fires.erase(m_fires.begin() + static_cast<std::ptrdiff_t>(m_cursor));
            } else ++m_cursor;
        }
    }

    template<typename Fn>
    void for_each_near(const glm::vec3& center, float radius, size_t limit, Fn&& visitor) const {
        const float squared = radius * radius;
        size_t visited = 0;
        for (const Flame& f : m_fires) {
            const glm::vec3 pos(static_cast<float>(f.pos.x) + .5f,
                                static_cast<float>(f.pos.y) + .5f,
                                static_cast<float>(f.pos.z) + .5f);
            const glm::vec3 d = pos - center;
            if (glm::dot(d, d) <= squared && ++visited <= limit) visitor(pos);
            if (visited >= limit) break;
        }
    }

    void clear() { m_fires.clear(); m_cursor = 0; m_accumulator = 0.0f; m_step = 0; }

    void save(std::ostream& out) const {
        for (const auto& f : m_fires)
            out << f.pos.x << ' ' << f.pos.y << ' ' << f.pos.z << ' ' << f.seconds << '\n';
    }
    void load(std::istream& in) {
        clear();
        Flame f;
        while (in >> f.pos.x >> f.pos.y >> f.pos.z >> f.seconds) {
            if (f.pos.y >= Config::world_min_y && f.pos.y <= Config::world_max_y &&
                f.seconds > 0 && f.seconds <= 60 && m_fires.size() < kMaxActive)
                track(f.pos, f.seconds);
        }
    }
private:
    static constexpr size_t kMaxActive = 2048;
    static constexpr size_t kPerSecondBudget = 128;
    inline static const glm::ivec3 kDirections[6] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    std::vector<Flame> m_fires;
    size_t m_cursor = 0;
    float m_accumulator = 0.f;
    uint32_t m_step = 0;
    static uint32_t hash(const glm::ivec3& p, uint32_t t) {
        uint32_t x = static_cast<uint32_t>(p.x) * 73856093u ^
                     static_cast<uint32_t>(p.y) * 19349663u ^
                     static_cast<uint32_t>(p.z) * 83492791u ^ t * 2654435761u;
        x ^= x >> 16u; x *= 0x7feb352du; x ^= x >> 15u; x *= 0x846ca68bu;
        return x ^ (x >> 16u);
    }
    static bool loaded(const glm::ivec3& p, Chunk_Manager& chunks) {
        if (p.y < Config::world_min_y || p.y > Config::world_max_y) return false;
        return chunks.get_chunk(Math_Helpers::floor_div(p.x, Config::chunk_size),
                                Math_Helpers::floor_div(p.y, Config::chunk_height),
                                Math_Helpers::floor_div(p.z, Config::chunk_size)) != nullptr;
    }
    void track(const glm::ivec3& p, int seconds) {
        for (auto& f : m_fires) if (f.pos == p) { f.seconds = std::max(f.seconds, seconds); return; }
        if (m_fires.size() < kMaxActive) m_fires.push_back({p, seconds});
    }
};
