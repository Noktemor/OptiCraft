#include "world/chunks/Chunk_Manager.h"
#include "world/generation/Region_Cache.h"
#include "world/generation/World_Generator.h"

#include <chrono>
#include <cstdio>
#include <future>
#include <thread>

int main(int, char**) {
    Region_Cache<int> cache;
    if (cache.try_get(1)) return 1;
    std::promise<void> started, release;
    auto gate = release.get_future().share();
    auto builder = std::async(std::launch::async, [&] {
        return cache.get(1, 1, [&] {
            started.set_value();
            gate.wait();
            return std::make_shared<int>(42);
        });
    });
    started.get_future().wait();
    const bool pending_is_empty = !cache.try_get(1);
    release.set_value();
    auto retained = builder.get();
    if (!pending_is_empty || !retained || *retained != 42 || !cache.try_get(1)) return 2;
    cache.get(2, 1, [] { return std::make_shared<int>(7); });
    if (cache.try_get(1) || *retained != 42) return 3;

    World_Generator generator("Classic", 123u, Dimension_Id::Overworld);
    constexpr int x = -513, z = 257;
    if (generator.try_biome_id(x, 64, z)) return 4;
    Chunk_Manager chunks(2);
    chunks.set_world_generator(generator);
    const auto c = chunks.get_chunk_coords_for_world(x, 64, z);
    chunks.request_generation_region(c.x, c.z);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!generator.try_biome_id(x, 64, z) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (!generator.try_biome_id(x, 64, z)) return 5;
    for (int y : {-151, -150, -80, -63, -60, 0, 64, 130, 131, 220}) {
        const auto fast = generator.try_biome_id(x, y, z);
        if (!fast || *fast != generator.biome_id(x, y, z)) return 6;
    }
    std::puts("PASS: cold and pending cache are nonblocking; eviction retains values; queued region yields matching biomes");
    return 0;
}
