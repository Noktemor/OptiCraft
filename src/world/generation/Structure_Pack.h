#ifndef OPTICRAFT_STRUCTURE_PACK_H
#define OPTICRAFT_STRUCTURE_PACK_H

// Immutable voxel templates. One OSTR archive is loaded once and shared by all
// worldgen workers. No disk access, queue, or allocation on individual placement.
#include "world/blocks/Block_Types.h"
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum class Structure_Template_Id : uint8_t {
    Cottage_Oak = 0, Cottage_Birch, Farm, Well, Fortress,
    Count
};

struct Structure_Voxel {
    int16_t x = 0, y = 0, z = 0;
    Block_Types type = Block_Types::Air;
    uint8_t meta = 0;
    uint8_t flags = 0; // bit 0: rotate directional meta
};

struct Structure_Template {
    std::string name;
    int width = 0, height = 0, depth = 0;
    int entrance_x = 0, entrance_z = 0; // rotation pivot, on the front (-Z) face
    std::vector<Structure_Voxel> voxels;
};

// A single large structure and a composite village use the same template instance.
struct Structure_Instance {
    Structure_Template_Id template_id = Structure_Template_Id::Cottage_Oak;
    int entrance_world_x = 0;
    int base_world_y = 0;
    int entrance_world_z = 0;
    uint8_t rotation = 0; // 0: -Z, 1: +X, 2: +Z, 3: -X
    std::shared_ptr<const Structure_Template> custom_template; // large procedural maze/dungeon adapter
};

// Useful for adapters to existing procedural builders (e.g. Maze_Generator::place).
// Coordinates from the callback become immutable voxel records; generation is NOT
// repeated for each intersecting chunk.
using Structure_Voxel_Buffer = std::vector<Structure_Voxel>;

class Structure_Pack {
public:
    static const Structure_Pack& instance();
    const Structure_Template* get(Structure_Template_Id id) const;
    bool ready() const { return m_ready; }

    static void rotate_xz(int x, int z, uint8_t rotation, int& out_x, int& out_z);
    static void world_position(const Structure_Template& t, const Structure_Instance& instance,
                               const Structure_Voxel& voxel, int& x, int& y, int& z);

private:
    Structure_Pack();
    bool m_ready = false;
    std::array<Structure_Template, static_cast<size_t>(Structure_Template_Id::Count)> m_templates{};
};

#endif
