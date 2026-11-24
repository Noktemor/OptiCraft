#pragma once
#include <array>
#include <cstdint>

namespace OptiCraft::Vulkan {
// Six inward-facing world-space planes, (normal.xyz, signed distance).
// Matrix input is column-major and uses Vulkan clip depth [0, 1].
// Extraction happens once per camera update; the GPU performs per-chunk tests.
struct alignas(16) GpuFrustum {
    std::array<std::array<float, 4>, 6> planes{};
    static GpuFrustum fromVulkanViewProjection(const float* columnMajor16);
    // Reference implementation for tests and conservative diagnostic checks.
    bool containsChunk(int32_t x, int32_t y, int32_t z, float side = 32.f) const noexcept;
};
static_assert(sizeof(GpuFrustum) == 96);
} // namespace OptiCraft::Vulkan
