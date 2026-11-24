#include "rendering/vulkan/GpuFrustum.h"
#include <cmath>
#include <stdexcept>

namespace OptiCraft::Vulkan {
GpuFrustum GpuFrustum::fromVulkanViewProjection(const float* m) {
    if (!m) throw std::invalid_argument("Null view-projection matrix");
    GpuFrustum result;
    auto row = [&](int r, int c) { return m[c * 4 + r]; };
    // left, right, bottom, top, near, far; Vulkan z ranges from 0 to w.
    for (int i = 0; i < 6; ++i) {
        for (int c = 0; c < 4; ++c) {
            const float w = row(3, c);
            switch (i) {
            case 0: result.planes[i][c] = w + row(0,c); break;
            case 1: result.planes[i][c] = w - row(0,c); break;
            case 2: result.planes[i][c] = w + row(1,c); break;
            case 3: result.planes[i][c] = w - row(1,c); break;
            case 4: result.planes[i][c] = row(2,c); break;
            default: result.planes[i][c] = w - row(2,c); break;
            }
        }
        auto& p = result.planes[i];
        const float norm = std::sqrt(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
        if (!std::isfinite(norm) || !std::isfinite(p[3]))
            throw std::invalid_argument("Non-finite view-projection matrix");
        if (norm < 1.e-20f) {
            // Infinite far plane / degenerate clipping plane: never cull by it.
            p = {0,0,0,0};
        } else {
            for (float& a : p) a /= norm;
        }
    }
    return result;
}
bool GpuFrustum::containsChunk(int32_t x, int32_t y, int32_t z, float side) const noexcept {
    for (const auto& p : planes) {
        const float px = float(x) + (p[0] >= 0 ? side : 0.f);
        const float py = float(y) + (p[1] >= 0 ? side : 0.f);
        const float pz = float(z) + (p[2] >= 0 ? side : 0.f);
        if (p[0]*px + p[1]*py + p[2]*pz + p[3] < 0.f) return false;
    }
    return true;
}
} // namespace OptiCraft::Vulkan
