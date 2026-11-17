#ifndef OPTICRAFT_STRUCTURE_ADAPTERS_H
#define OPTICRAFT_STRUCTURE_ADAPTERS_H

// Adapter only: existing Maze_Generator and Dungeon_Layout stay unchanged.
// Capture them ONCE into an immutable template, then pass a shared_ptr through
// Structure_Instance::custom_template to the normal chunk-clipped placer.
#include "world/generation/Structure_Pack.h"
#include "world/generation/Maze.h"
#include "world/structures/Dungeon_Layout.h"

inline Structure_Template make_maze_structure_template(const Maze& maze) {
    const Maze_Size dimensions = Maze_Generator::size_of(maze);
    Structure_Template t;
    t.name = "Generated_Maze";
    t.width = dimensions.x;
    t.height = dimensions.y;
    t.depth = dimensions.z;
    t.entrance_x = dimensions.x / 2;
    t.entrance_z = 0;
    Maze_Generator::place(maze, 0, 0, 0,
        [&](int x, int y, int z, Block_Types type, uint8_t meta) {
            t.voxels.push_back({static_cast<int16_t>(x), static_cast<int16_t>(y),
                static_cast<int16_t>(z), type, meta,
                static_cast<uint8_t>(type == Block_Types::Stone_Stairs ? 1 : 0)});
        });
    return t;
}

inline Structure_Template make_dungeon_structure_template(const Dungeon_Layout& layout) {
    Structure_Template t;
    t.name = "Generated_Dungeon";
    t.width = layout.size_x;
    t.height = layout.size_y;
    t.depth = layout.size_z;
    t.entrance_x = layout.size_x / 2;
    t.entrance_z = 0;
    t.voxels.reserve(layout.blocks.size());
    for (const Dungeon_Block& b : layout.blocks)
        t.voxels.push_back({static_cast<int16_t>(b.x), static_cast<int16_t>(b.y),
            static_cast<int16_t>(b.z), b.type, 0, 0});
    return t;
}

#endif
