#pragma once

// Per-world list of portal entrances. Coordinates are the bottom, first inner
// cell of a 4x5 Nether frame or the center of an End portal. The registry
// belongs to the save, not to a streaming Chunk_Manager instance.
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

struct Portal_Site {
    std::string dimension;
    glm::ivec3 origin{0};
    bool along_z = false;
    bool end_portal = false;
};

class Portal_Link_Registry {
public:
    void clear() { m_sites.clear(); }
    bool remember(const Portal_Site& entry) {
        if (entry.dimension.empty()) return false;
        for (const auto& e : m_sites)
            if (e.dimension == entry.dimension && e.origin == entry.origin &&
                e.end_portal == entry.end_portal) return false;
        m_sites.push_back(entry);
        return true;
    }
    bool forget(const Portal_Site& site) {
        const size_t count = m_sites.size();
        m_sites.erase(std::remove_if(m_sites.begin(), m_sites.end(), [&](const Portal_Site& e) {
            return e.dimension == site.dimension && e.origin == site.origin &&
                   e.end_portal == site.end_portal;
        }), m_sites.end());
        return m_sites.size() != count;
    }
    const Portal_Site* nearest(const std::string& dimension, float x, float z,
                               bool end_portal, float radius) const {
        const Portal_Site* best = nullptr;
        float best_dist_sq = radius * radius;
        for (const auto& e : m_sites) {
            if (e.dimension != dimension || e.end_portal != end_portal) continue;
            const float dx = static_cast<float>(e.origin.x) - x;
            const float dz = static_cast<float>(e.origin.z) - z;
            const float distance_sq = dx * dx + dz * dz;
            if (distance_sq <= best_dist_sq) { best = &e; best_dist_sq = distance_sq; }
        }
        return best;
    }
    // Inside the portal so players see the expected exit; the exit lock keeps
    // them from being immediately teleported back until they leave the plane.
    static glm::vec3 entry_position(const Portal_Site& e) {
        return {static_cast<float>(e.origin.x) + 0.5f,
                static_cast<float>(e.origin.y) + (e.end_portal ? 1.2f : 1.0f),
                static_cast<float>(e.origin.z) + 0.5f};
    }
    void save(std::ostream& out) const {
        for (const auto& e : m_sites)
            out << std::quoted(e.dimension) << ' ' << e.origin.x << ' ' << e.origin.y << ' '
                << e.origin.z << ' ' << e.along_z << ' ' << e.end_portal << '\n';
    }
    void load(std::istream& in) {
        clear();
        Portal_Site e;
        while (in >> std::quoted(e.dimension) >> e.origin.x >> e.origin.y >> e.origin.z
                  >> e.along_z >> e.end_portal) {
            if (m_sites.size() >= 16384) break;
            remember(e);
        }
    }
private:
    std::vector<Portal_Site> m_sites;
};
