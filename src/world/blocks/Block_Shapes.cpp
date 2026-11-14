#include "world/blocks/Block_Shapes.h"

uint8_t stairs_high_side(uint8_t meta) {
    return static_cast<uint8_t>(meta & 3u);
}

uint8_t opposite_facing(uint8_t meta) {
    return static_cast<uint8_t>((meta + 2u) & 3u);
}

uint8_t stairs_effective_meta(uint8_t meta, bool front_is_stairs, uint8_t front_meta,
                              bool back_is_stairs, uint8_t back_meta) {
    const uint8_t facing = static_cast<uint8_t>(meta & 3u);
    const uint8_t left = static_cast<uint8_t>((facing + 3u) & 3u);
    const uint8_t right = static_cast<uint8_t>((facing + 1u) & 3u);

    // A perpendicular stair in front makes an outer corner. A perpendicular stair behind
    // makes an inner corner. Front takes precedence in a compact 2x2 stair junction.
    uint8_t corner = 0u;
    uint8_t side = 0u; // 0 = left, 1 = right
    if (front_is_stairs) {
        const uint8_t neighbor_facing = static_cast<uint8_t>(front_meta & 3u);
        if (neighbor_facing == left) { corner = 1u; side = 0u; }
        else if (neighbor_facing == right) { corner = 1u; side = 1u; }
    }
    if (corner == 0u && back_is_stairs) {
        const uint8_t neighbor_facing = static_cast<uint8_t>(back_meta & 3u);
        if (neighbor_facing == left) { corner = 2u; side = 0u; }
        else if (neighbor_facing == right) { corner = 2u; side = 1u; }
    }
    return static_cast<uint8_t>(facing | (corner << 2u) | (side << 4u));
}

bool shapes_connect(Block_Shape own, Block_Shape neighbor, bool full_cube) {
    if (full_cube) return true;
    if (own == Block_Shape::Fence) return neighbor == Block_Shape::Fence || neighbor == Block_Shape::FenceGate;
    if (own == Block_Shape::Wall) return neighbor == Block_Shape::Wall;
    if (own == Block_Shape::Pane) return neighbor == Block_Shape::Pane;
    return false;
}

uint8_t shape_connection_meta(Block_Shape, bool north, bool east, bool south, bool west) {
    return static_cast<uint8_t>((north ? 1u : 0u) | (east ? 2u : 0u) |
                                (south ? 4u : 0u) | (west ? 8u : 0u));
}

int shape_boxes(Block_Shape shape, uint8_t meta, Shape_Box out[16]) {
    switch (shape) {
        case Block_Shape::Slab:
            out[0] = {glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.5f, 1.0f)};
            return 1;
        case Block_Shape::Stairs: {
            // Four lower quarter-columns cover the bottom half. The upper footprint is a
            // two-quarter strip (straight), one quarter (outer corner), or three quarters
            // (inner corner). Keeping quarters separate lets the mesh shader cull every
            // shared internal face generically; it avoids coplanar overlap/z-fighting.
            constexpr float half = 0.5f;
            for (int qz = 0; qz < 2; ++qz) {
                for (int qx = 0; qx < 2; ++qx) {
                    out[qz * 2 + qx] = {
                        glm::vec3(qx * half, 0.0f, qz * half),
                        glm::vec3((qx + 1) * half, half, (qz + 1) * half)
                    };
                }
            }

            const uint8_t facing = static_cast<uint8_t>(meta & 3u);
            const uint8_t corner = static_cast<uint8_t>((meta >> 2u) & 3u);
            const bool right_side = (meta & 16u) != 0u;
            uint8_t facing_mask = 0u;
            uint8_t side_mask = 0u;
            // Quadrant bits: 0=NW, 1=NE, 2=SW, 3=SE.
            switch (facing) {
                case Block_Meta::facing_north: facing_mask = 0x3u; break;
                case Block_Meta::facing_east:  facing_mask = 0xAu; break;
                case Block_Meta::facing_south: facing_mask = 0xCu; break;
                default:                       facing_mask = 0x5u; break;
            }
            const uint8_t side_direction = right_side
                ? static_cast<uint8_t>((facing + 1u) & 3u)
                : static_cast<uint8_t>((facing + 3u) & 3u);
            switch (side_direction) {
                case Block_Meta::facing_north: side_mask = 0x3u; break;
                case Block_Meta::facing_east:  side_mask = 0xAu; break;
                case Block_Meta::facing_south: side_mask = 0xCu; break;
                default:                       side_mask = 0x5u; break;
            }

            uint8_t upper_mask = facing_mask;
            if (corner == 1u) upper_mask = static_cast<uint8_t>(facing_mask & side_mask); // outer L-turn tip
            else if (corner == 2u) upper_mask = static_cast<uint8_t>(facing_mask | side_mask); // inner L-fill
            // Compact upper boxes so the mesh shader never scans holes in the output array.
            int count = 4;
            for (int quadrant = 0; quadrant < 4; ++quadrant) {
                if ((upper_mask & (1u << quadrant)) == 0u) continue;
                const int qx = quadrant & 1;
                const int qz = quadrant >> 1;
                out[count++] = {glm::vec3(qx * half, half, qz * half),
                                glm::vec3((qx + 1) * half, 1.0f, (qz + 1) * half)};
            }
            return count;
        }

        case Block_Shape::Plate:
            // Пластинка 1/16 толщиной у пола (медный провод).
            out[0] = {glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f, 1.0f / 16.0f, 1.0f)};
            return 1;
        case Block_Shape::Wire:
            // Провод: 12 на 12 пикселей текстуры, высота 2 пикселя, по центру клетки у пола.
            out[0] = {glm::vec3(2.0f / 16.0f, 0.0f, 2.0f / 16.0f), glm::vec3(14.0f / 16.0f, 2.0f / 16.0f, 14.0f / 16.0f)};
            return 1;
        case Block_Shape::Button: {
            // Кнопка 3 (ширина) x 2 (высота) x 2 (вынос от опоры) пикселя по центру грани крепления.
            // meta & 7 — индекс стороны опоры: +X, -X, +Y, -Y, +Z, -Z (как kDirs в Logic_Manager).
            constexpr float p = 1.0f / 16.0f;
            constexpr float w0 = 6.5f * p, w1 = 9.5f * p;   // ширина 3 пикселя
            constexpr float h0 = 7.0f * p, h1 = 9.0f * p;   // высота / глубина 2 пикселя
            switch (meta & 7u) {
                case 0:  out[0] = {glm::vec3(1.0f - 2.0f * p, h0, w0), glm::vec3(1.0f, h1, w1)}; break;          // опора +X
                case 1:  out[0] = {glm::vec3(0.0f, h0, w0), glm::vec3(2.0f * p, h1, w1)}; break;                 // опора -X
                case 2:  out[0] = {glm::vec3(w0, 1.0f - 2.0f * p, h0), glm::vec3(w1, 1.0f, h1)}; break;          // опора +Y
                case 4:  out[0] = {glm::vec3(w0, h0, 1.0f - 2.0f * p), glm::vec3(w1, h1, 1.0f)}; break;          // опора +Z
                case 5:  out[0] = {glm::vec3(w0, h0, 0.0f), glm::vec3(w1, h1, 2.0f * p)}; break;                 // опора -Z
                default: out[0] = {glm::vec3(w0, 0.0f, h0), glm::vec3(w1, 2.0f * p, h1)}; break;                 // опора -Y (пол)
            }
            return 1;
        }
        // All dimensions below are multiples of 1/16 (vanilla pixel coordinates). The GPU
        // stores them as 1/64, so the same boxes drive rendering, collision and ray selection.
        case Block_Shape::Fence:
        case Block_Shape::Wall:
        case Block_Shape::Pane: {
            constexpr float p = 1.0f / 16.0f;
            int n = 0;
            const bool wall = shape == Block_Shape::Wall;
            const bool pane = shape == Block_Shape::Pane;
            const float center = pane ? 7.0f : wall ? 4.0f : 6.0f;
            const float end = 16.0f - center;
            out[n++] = {glm::vec3(center*p, 0.0f, center*p),
                        glm::vec3(end*p, (pane ? 16.0f : 16.0f)*p, end*p)};
            for (int dir = 0; dir < 4; ++dir) {
                if ((meta & (1u << dir)) == 0u) continue;
                const float x0 = dir == 1 ? end : dir == 3 ? 0.0f : center;
                const float x1 = dir == 1 ? 16.0f : dir == 3 ? center : end;
                const float z0 = dir == 2 ? end : dir == 0 ? 0.0f : center;
                const float z1 = dir == 2 ? 16.0f : dir == 0 ? center : end;
                if (pane || wall) {
                    out[n++] = {glm::vec3(x0*p, 0.0f, z0*p),
                                glm::vec3(x1*p, (wall ? 13.0f : 16.0f)*p, z1*p)};
                } else {
                    out[n++] = {glm::vec3(x0*p, 6*p, z0*p), glm::vec3(x1*p, 9*p, z1*p)};
                    out[n++] = {glm::vec3(x0*p, 12*p, z0*p), glm::vec3(x1*p, 15*p, z1*p)};
                }
            }
            return n;
        }
        case Block_Shape::FenceGate: {
            constexpr float p = 1.0f / 16.0f;
            // Closed gate has two posts and horizontal bars. Open gate folds both leaves
            // against their posts; meta bit 2 is open and bits 0..1 are facing.
            const bool open = (meta & 4u) != 0u;
            int n = 0;
            auto add = [&](float x0,float y0,float z0,float x1,float y1,float z1) {
                Shape_Box b{glm::vec3(x0*p,y0*p,z0*p),glm::vec3(x1*p,y1*p,z1*p)};
                for (int k=0; k<(meta&3u); ++k) {
                    const float a=b.lo.x, c=b.hi.x;
                    b.lo.x=b.lo.z; b.hi.x=b.hi.z;
                    b.lo.z=1.0f-c; b.hi.z=1.0f-a;
                }
                out[n++]=b;
            };
            add(0,0,6,2,16,10); add(14,0,6,16,16,10);
            if (open) {
                add(0,5,10,2,8,16); add(14,5,10,16,8,16);
                add(0,11,10,2,14,16); add(14,11,10,16,14,16);
            } else {
                add(2,5,7,14,8,9); add(2,11,7,14,14,9);
            }
            return n;
        }
        case Block_Shape::Door: {
            // One half per cell, metadata bit 3 marks upper half (both have the same shape).
            // Open state swings against the near edge instead of disappearing.
            const bool open = (meta & 4u) != 0u;
            Shape_Box b = open
                ? Shape_Box{glm::vec3(0,0,0),glm::vec3(3.0f/16.0f,1,1)}
                : Shape_Box{glm::vec3(0,0,0),glm::vec3(1,1,3.0f/16.0f)};
            for (int k=0;k<(meta&3u);++k) {
                const float a=b.lo.x,c=b.hi.x;
                b.lo.x=b.lo.z;b.hi.x=b.hi.z;b.lo.z=1.0f-c;b.hi.z=1.0f-a;
            }
            out[0]=b;return 1;
        }
        case Block_Shape::Trapdoor: {
            const bool open = (meta & 4u) != 0u;
            Shape_Box b = open
                ? Shape_Box{glm::vec3(0,0,0),glm::vec3(1,1,3.0f/16.0f)}
                : ((meta & 8u) != 0u
                    ? Shape_Box{glm::vec3(0,13.0f/16.0f,0),glm::vec3(1,1,1)}
                    : Shape_Box{glm::vec3(0,0,0),glm::vec3(1,3.0f/16.0f,1)});
            if (open) for(int k=0;k<(meta&3u);++k) {
                const float a=b.lo.x,c=b.hi.x;
                b.lo.x=b.lo.z;b.hi.x=b.hi.z;b.lo.z=1.0f-c;b.hi.z=1.0f-a;
            }
            out[0]=b;return 1;
        }
        case Block_Shape::PressurePlate:
            out[0] = {glm::vec3(1.0f/16,0,1.0f/16),glm::vec3(15.0f/16,(meta&4u)?0.5f/16:1.0f/16,15.0f/16)};
            return 1;
        case Block_Shape::Ladder: {
            // Wall attachment: a thin panel, 2 bits facing.
            out[0] = {glm::vec3(0,0,0),glm::vec3(1,1,1.0f/16)};
            for (int k=0;k<(meta&3u);++k) {
                Shape_Box& b=out[0];const float a=b.lo.x,c=b.hi.x;
                b.lo.x=b.lo.z;b.hi.x=b.hi.z;b.lo.z=1.0f-c;b.hi.z=1.0f-a;
            }
            return 1;
        }
        case Block_Shape::Carpet:
            out[0] = {glm::vec3(0),glm::vec3(1,1.0f/16,1)};return 1;
        case Block_Shape::SnowLayer:
            out[0] = {glm::vec3(0),glm::vec3(1,static_cast<float>(((meta&7u)+1u))/8.0f,1)};return 1;
        case Block_Shape::Sign:
        case Block_Shape::HangingSign: {
            const bool hanging = shape == Block_Shape::HangingSign;
            int n=0;
            // Rotate sign board, and place pole/chain under or above it.
            Shape_Box board = hanging
                ? Shape_Box{glm::vec3(0,4.0f/16,7.0f/16),glm::vec3(1,12.0f/16,9.0f/16)}
                : Shape_Box{glm::vec3(0,7.0f/16,7.0f/16),glm::vec3(1,15.0f/16,9.0f/16)};
            for (int k=0;k<(meta&3u);++k) {
                const float a=board.lo.x,c=board.hi.x;
                board.lo.x=board.lo.z;board.hi.x=board.hi.z;
                board.lo.z=1.0f-c;board.hi.z=1.0f-a;
            }
            out[n++]=board;
            if (hanging) {
                out[n++]={glm::vec3(3.0f/16,12.0f/16,7.0f/16),glm::vec3(4.0f/16,1,9.0f/16)};
                out[n++]={glm::vec3(12.0f/16,12.0f/16,7.0f/16),glm::vec3(13.0f/16,1,9.0f/16)};
            } else out[n++]={glm::vec3(7.0f/16,0,7.0f/16),glm::vec3(9.0f/16,7.0f/16,9.0f/16)};
            return n;
        }
        case Block_Shape::Rail:
            out[0]={glm::vec3(0,0,0),glm::vec3(1,1.0f/16,1)};return 1;
        case Block_Shape::Chain:
            out[0]={glm::vec3(7.0f/16,0,7.0f/16),glm::vec3(9.0f/16,1,9.0f/16)};return 1;
        case Block_Shape::Lantern:
            out[0]={glm::vec3(5.0f/16,2.0f/16,5.0f/16),glm::vec3(11.0f/16,11.0f/16,11.0f/16)};
            out[1]={glm::vec3(7.0f/16,11.0f/16,7.0f/16),glm::vec3(9.0f/16,15.0f/16,9.0f/16)};
            out[2]={glm::vec3(4.0f/16,1.0f/16,4.0f/16),glm::vec3(12.0f/16,2.0f/16,12.0f/16)};
            return 3;
        case Block_Shape::Cauldron: {
            constexpr float p=1.0f/16;
            out[0]={glm::vec3(0,0,0),glm::vec3(1,3*p,1)};
            out[1]={glm::vec3(0,3*p,0),glm::vec3(2*p,1,1)};
            out[2]={glm::vec3(14*p,3*p,0),glm::vec3(1,1,1)};
            out[3]={glm::vec3(2*p,3*p,0),glm::vec3(14*p,1,2*p)};
            out[4]={glm::vec3(2*p,3*p,14*p),glm::vec3(14*p,1,1)};
            return 5;
        }
        case Block_Shape::Hopper: {
            constexpr float p=1.0f/16;
            out[0]={glm::vec3(2*p,0,2*p),glm::vec3(14*p,10*p,14*p)};
            out[1]={glm::vec3(0,10*p,0),glm::vec3(2*p,1,1)};
            out[2]={glm::vec3(14*p,10*p,0),glm::vec3(1,1,1)};
            out[3]={glm::vec3(2*p,10*p,0),glm::vec3(14*p,1,2*p)};
            out[4]={glm::vec3(2*p,10*p,14*p),glm::vec3(14*p,1,1)};
            out[5]={glm::vec3(6*p,0,6*p),glm::vec3(10*p,2*p,10*p)};
            return 6;
        }
        case Block_Shape::Bed:
            out[0]={glm::vec3(0,3.0f/16,0),glm::vec3(1,9.0f/16,1)};
            out[1]={glm::vec3(1.0f/16,0,1.0f/16),glm::vec3(3.0f/16,3.0f/16,3.0f/16)};
            out[2]={glm::vec3(13.0f/16,0,1.0f/16),glm::vec3(15.0f/16,3.0f/16,3.0f/16)};
            out[3]={glm::vec3(1.0f/16,0,13.0f/16),glm::vec3(3.0f/16,3.0f/16,15.0f/16)};
            out[4]={glm::vec3(13.0f/16,0,13.0f/16),glm::vec3(15.0f/16,3.0f/16,15.0f/16)};
            return 5;
        case Block_Shape::Candle:
            out[0]={glm::vec3(7.0f/16,0,7.0f/16),glm::vec3(9.0f/16,7.0f/16,9.0f/16)};return 1;
        case Block_Shape::Rod:
            out[0]={glm::vec3(7.0f/16,0,7.0f/16),glm::vec3(9.0f/16,1,9.0f/16)};return 1;
        case Block_Shape::FlowerPot:
            out[0]={glm::vec3(5.0f/16,0,5.0f/16),glm::vec3(11.0f/16,1.0f/16,11.0f/16)};
            out[1]={glm::vec3(5.0f/16,1.0f/16,5.0f/16),glm::vec3(6.0f/16,6.0f/16,11.0f/16)};
            out[2]={glm::vec3(10.0f/16,1.0f/16,5.0f/16),glm::vec3(11.0f/16,6.0f/16,11.0f/16)};
            out[3]={glm::vec3(6.0f/16,1.0f/16,5.0f/16),glm::vec3(10.0f/16,6.0f/16,6.0f/16)};
            out[4]={glm::vec3(6.0f/16,1.0f/16,10.0f/16),glm::vec3(10.0f/16,6.0f/16,11.0f/16)};
            return 5;
        case Block_Shape::Bell:
            out[0]={glm::vec3(5.0f/16,2.0f/16,5.0f/16),glm::vec3(11.0f/16,11.0f/16,11.0f/16)};
            out[1]={glm::vec3(7.0f/16,11.0f/16,7.0f/16),glm::vec3(9.0f/16,1,9.0f/16)};return 2;
        case Block_Shape::Grindstone:
            out[0]={glm::vec3(2.0f/16,0,5.0f/16),glm::vec3(4.0f/16,12.0f/16,11.0f/16)};
            out[1]={glm::vec3(12.0f/16,0,5.0f/16),glm::vec3(14.0f/16,12.0f/16,11.0f/16)};
            out[2]={glm::vec3(4.0f/16,4.0f/16,3.0f/16),glm::vec3(12.0f/16,14.0f/16,13.0f/16)};return 3;
        case Block_Shape::Lectern:
            out[0]={glm::vec3(6.0f/16,0,6.0f/16),glm::vec3(10.0f/16,12.0f/16,10.0f/16)};
            out[1]={glm::vec3(2.0f/16,12.0f/16,2.0f/16),glm::vec3(14.0f/16,15.0f/16,14.0f/16)};
            out[2]={glm::vec3(4.0f/16,0,4.0f/16),glm::vec3(12.0f/16,2.0f/16,12.0f/16)};return 3;
        case Block_Shape::TorchPost:
            out[0]={glm::vec3(7.0f/16,0,7.0f/16),glm::vec3(9.0f/16,10.0f/16,9.0f/16)};
            out[1]={glm::vec3(6.0f/16,10.0f/16,6.0f/16),glm::vec3(10.0f/16,12.0f/16,10.0f/16)};
            return 2;
        case Block_Shape::None:
        case Block_Shape::Count:
        default:
            return 0;
    }
}
