#pragma once

#include <algorithm>

// Residency profile for large view distances. The previous rectangular X/Z/Y
// volume retained up to (2R+1)^2 * (2H+1) chunks, even for distant columns that
// occupy only a tiny fraction of the screen. Keep full vertical detail close to
// the player, then gradually reduce the vertical window for distant columns.
// Small/medium render distances preserve their original behaviour.
namespace Chunk_Stream_Window {
    constexpr int vertical_half(int horizontal_radius, int requested_half, int horizontal_ring) {
        if (horizontal_radius < 24) return requested_half;
        if (horizontal_ring <= std::max(6, horizontal_radius / 5)) return requested_half;
        if (horizontal_ring <= (horizontal_radius * 3) / 5) return std::min(requested_half, 1);
        return 0;
    }

    constexpr bool wanted(int horizontal_radius, int requested_half,
                          int dx, int dy, int dz) {
        if (dx > horizontal_radius || dz > horizontal_radius) return false;
        return dy <= vertical_half(horizontal_radius, requested_half, std::max(dx, dz));
    }
}
