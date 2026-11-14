#ifndef OPTICRAFT_GPU_WORLDGEN_H
#define OPTICRAFT_GPU_WORLDGEN_H

#include "world/generation/Gpu_Worldgen_Data.h"
#include "utils/configs/Config_World.h"
#include <array>
#include <cstdint>
#include <cstddef>
#include <vector>

class World_Generator;

// All GL calls must happen on the owning render/main thread. No worker accesses GL.
// This optional backend computes only deep material IDs: procedural terrain, caves,
// rivers, structures, sunlight, palette packing and disk I/O remain on CPU.
class Gpu_Worldgen {
public:
    Gpu_Worldgen() = default;
    ~Gpu_Worldgen();
    Gpu_Worldgen(const Gpu_Worldgen&) = delete;
    Gpu_Worldgen& operator=(const Gpu_Worldgen&) = delete;

    bool start(const World_Generator& generator, int chunk_x, int chunk_z,
        const std::array<Gpu_Material_Column, Config::chunk_size * Config::chunk_size>& columns,
        const std::vector<int>& chunk_ys);
    // Return true only when the complete readback is ready; never blocks for a fence.
    bool poll(std::vector<uint32_t>& output, double& gpu_ms);
    void abandon();
    bool enabled() const { return !m_disabled; }
    bool busy() const { return m_sync != nullptr; }
private:
    bool initialize(const World_Generator& generator);
    void shutdown();
    unsigned int m_program = 0;
    unsigned int m_static_buffers[4]{};
    unsigned int m_column_buffer = 0;
    unsigned int m_levels_buffer = 0;
    unsigned int m_output_buffer = 0;
    unsigned int m_timer_query = 0;
    void* m_sync = nullptr; // GLsync; kept opaque in header
    size_t m_count = 0;
    bool m_initialized = false;
    bool m_disabled = false;
};

#endif
