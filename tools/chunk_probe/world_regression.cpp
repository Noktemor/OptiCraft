#include "world/chunks/Chunk_Manager.h"
#include "world/generation/World_Generator.h"
#include "rendering/atlas/Texture_Atlas.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::filesystem::create_directories(argv[1]);
    World_Generator generator("Classic", 123u, Dimension_Id::Overworld);
    World_File file(std::filesystem::path(argv[1]) / "world.json");
    Texture_Atlas atlas;
    Chunk_Manager cm(2);
    cm.set_world_generator(generator);
    cm.set_world_storage(file, "Classic");
    cm.set_texture_atlas(atlas);
    for (int n = 0; n < 5000 && !(cm.get_chunk(0, 6, 0) && cm.get_chunk(1, 6, 0)); ++n) {
        cm.update_player_position(31.0f, 202.0f, 10.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!cm.get_chunk(0, 6, 0) || !cm.get_chunk(1, 6, 0)) { std::fprintf(stderr,"chunk load timeout\n"); return 3; }
    auto sky = [&]() { return cm.get_chunk(0,6,0)->get_sky_light(20,17,10); };
    for (int i=0; i<200; ++i) cm.collect_finished_chunks();
    const int before=sky();
    if (!cm.set_block_world(20,210,10,Block_Types::Stone)) return 9;
    for (int i=0; i<200; ++i) cm.collect_finished_chunks();
    const int blocked=sky();
    if (!cm.set_block_world(20,210,10,Block_Types::Air)) return 10;
    for (int i=0; i<200; ++i) cm.collect_finished_chunks();
    const int restored=sky();
    std::printf("sky before=%d blocked=%d restored=%d\n",before,blocked,restored);
    const bool sky_ok = before == 15 && blocked < before && restored == before;
    for (int x=27; x<=36; ++x) for (int z=5; z<=14; ++z)
        if (!cm.set_block_world(x, 200, z, Block_Types::Stone)) return 4;
    if (!cm.set_block_world(31, 202, 10, Block_Types::Water)) return 5;
    for (int i=0; i<100; ++i) cm.update_fluids(0.05f);
    const bool down = cm.get_block_world(31, 201, 10) == Block_Types::Water;
    const bool across = cm.get_block_world(32, 201, 10) == Block_Types::Water;
    unsigned long long hash = 1469598103934665603ull;
    int count = 0;
    for (int y=199; y<=203; ++y) for (int z=5; z<=14; ++z) for (int x=27; x<=36; ++x) {
        const auto b=cm.get_block_world(x,y,z);
        if (b==Block_Types::Water) ++count;
        hash = (hash ^ static_cast<unsigned>(b)) * 1099511628211ull;
        hash = (hash ^ cm.get_block_meta_world(x,y,z)) * 1099511628211ull;
    }
    std::printf("spread down=%d across=%d water=%d hash=%llu\n", down, across, count, hash);
    if (!down || !across) return 6;
    if (!cm.set_block_world(31, 202, 10, Block_Types::Air)) return 7;
    for (int i=0; i<250; ++i) cm.update_fluids(0.05f);
    const bool receded = cm.get_block_world(31, 201, 10) != Block_Types::Water && cm.get_block_world(32, 201, 10) != Block_Types::Water;
    std::printf("receded=%d\n", receded);
    if (!receded) return 8;
    return sky_ok ? 0 : 11;
}
