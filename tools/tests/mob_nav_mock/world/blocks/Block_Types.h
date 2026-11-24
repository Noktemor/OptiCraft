#pragma once
// Isolated navigation test mock; keep the interface aligned with production.
enum class Block_Types { Air, Stone, Water, Lava, Fire, Cactus, Door };
enum class Block_Shape { None, Door };
struct Block_Properties { bool is_solid=false; Block_Shape shape=Block_Shape::None; };
inline Block_Properties get_block_props(Block_Types b) {
    return {b==Block_Types::Stone || b==Block_Types::Cactus || b==Block_Types::Door,
            b==Block_Types::Door ? Block_Shape::Door : Block_Shape::None};
}
