#include "rendering/vulkan/GpuWorldCore.h"
#include "rendering/vulkan/GpuFrustum.h"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

using namespace OptiCraft::Vulkan;

int main() {
    std::vector<uint16_t> blockIds(kChunkVoxels);
    std::vector<uint32_t> packed(kChunkBlockWords);
    for (size_t i = 0; i < blockIds.size(); ++i) blockIds[i] = uint16_t((i * 397) % 65536);
    packBlockIds(blockIds, packed);
    for (size_t i = 0; i < blockIds.size(); ++i) {
        const auto word = packed[i >> 1];
        const auto id = (i & 1) ? (word >> 16) : (word & 65535u);
        assert(id == blockIds[i]);
    }
    // Alignment, best-fit, merge and double-free/overlap guards.
    RangeArena arena(2048);
    auto a = arena.allocate(80, 64);
    auto b = arena.allocate(100, 256);
    assert(a && b && a->offset == 0 && b->offset == 256);
    assert(arena.used() == 180);
    assert(arena.owns(*a) && arena.owns(*b));
    assert(!arena.owns({a->offset, a->size-1}));
    assert(!arena.release({a->offset, a->size-1}));
    assert(!arena.release({1300,20}));
    assert(arena.release(*a));
    assert(!arena.release(*a));
    assert(arena.release(*b));
    assert(arena.used() == 0 && arena.freeFragments() == 1);
    assert(!arena.allocate(2049));
    assert(!arena.allocate(16, 3));
    assert(arena.allocate(2048, 256));

    ChunkRegistry registry(4096, 4096);
    ChunkKey key{0, -2, 7, 999};
    auto h = registry.ensure(key, Authority::GpuProcedural);
    assert(h == registry.ensure(key, Authority::GpuProcedural));
    assert(registry.lookup(key) == h);
    assert(registry.resolve(h)->phase == ChunkPhase::NeedsGpuGeneration);
    auto blocks = registry.blocks().allocate(512);
    auto mesh1 = registry.meshes().allocate(640);
    assert(blocks && mesh1);
    assert(registry.setBlocks(h, *blocks, 4));
    assert(registry.publishMesh(h, *mesh1, 1, 6, 6));
    auto mesh2 = registry.meshes().allocate(512);
    assert(mesh2);
    assert(registry.publishMesh(h, *mesh2, 2, 9, 10));
    registry.collect(9);
    assert(registry.meshes().used() == 1152); // previous mesh in-flight until 10
    registry.collect(10);
    assert(registry.meshes().used() == 512);
    assert(registry.requestPromotion(h));
    assert(registry.finishPromotion(h));
    assert(registry.resolve(h)->authority == Authority::CpuAuthoritative);
    assert(registry.retire(h, 17));
    assert(!registry.resolve(h) && !registry.lookup(key));
    auto other = registry.ensure({0, 2, 7, 999}, Authority::GpuProcedural);
    assert(other.slot != h.slot); // slot remains retired until GPU is finished
    registry.collect(16);
    assert(registry.blocks().used() == 512);
    registry.collect(17);
    assert(registry.blocks().used() == 0 && registry.meshes().used() == 0);
    auto recycled = registry.ensure(key, Authority::GpuProcedural);
    assert(recycled.slot == h.slot && recycled.generation != h.generation);
    assert(!registry.resolve(h));

    // Coalesce by handle+kind; no duplicate GPU submissions when updates flood in.
    WorkBatcher jobs;
    GpuWork one{recycled, WorkKind::CountMesh, 1, 128, 100, 10};
    jobs.enqueue(std::span<const GpuWork>(&one, 1));
    one.version = 2; one.priority = 30; one.distanceSquared = 50;
    jobs.enqueue(std::span<const GpuWork>(&one, 1));
    assert(jobs.pending() == 1);
    assert(jobs.take(3, 100).empty()); // insufficient budget, no illegal overdispatch
    assert(jobs.pending() == 1);
    auto result = jobs.take(1, 128);
    assert(result.size() == 1 && result[0].version == 2 && result[0].priority == 30);
    assert(jobs.pending() == 0);

    std::vector<std::thread> producers;
    for (int i = 0; i < 8; ++i) {
        producers.emplace_back([&, i] {
            std::vector<GpuWork> many;
            for (int j = 0; j < 500; ++j)
                many.push_back({recycled, WorkKind::BuildLod, uint64_t(i * 500 + j),
                                1, uint32_t(j), 10});
            jobs.enqueue(many);
        });
    }
    for (auto& t : producers) t.join();
    assert(jobs.pending() == 1);
    auto last = jobs.take(2, 2);
    assert(last.size() == 1 && last[0].version == 3999);

    // Stress discontiguous GPU arena allocations with deterministic fragmentation.
    std::mt19937 random(1327);
    RangeArena fragmented(1024 * 1024);
    std::vector<BufferRange> live;
    for (int i = 0; i < 15000; ++i) {
        if (!live.empty() && (random() % 3 == 0)) {
            size_t index = random() % live.size();
            assert(fragmented.release(live[index]));
            live[index] = live.back();
            live.pop_back();
        } else if (auto range = fragmented.allocate(1 + random() % 512, 1u << (random() % 7))) {
            live.push_back(*range);
        }
        uint64_t total = 0;
        for (const auto& r : live) total += r.size;
        assert(total == fragmented.used());
    }
    for (auto range : live) assert(fragmented.release(range));
    assert(fragmented.used() == 0 && fragmented.freeFragments() == 1);

    // A retired handle must never reach Vulkan even if a worker submitted
    // the task before the stream window moved elsewhere.
    GpuWork stale{h, WorkKind::GenerateFullChunk, 1, 1, 0, 30};
    jobs.enqueue(std::span<const GpuWork>(&stale, 1));
    assert(jobs.take(16, 100, &registry).empty());
    assert(jobs.pending() == 0);

    GraphicsDistance distance{48, 12};
    assert(distance.cpuOwned(12, 0) && !distance.cpuOwned(13, 0));
    assert(distance.visible(40, 0) && !distance.visible(49, 0));
    assert(distance.visible(-48, 0) && !distance.visible(-49, 0));
    distance.actualChunks = 100; distance.normalize();
    assert(distance.actualChunks == 48);
    // Vulkan Z clip: [0,w], XY [-w,w]. Identity VP admits a chunk at 0,
    // rejects a chunk completely beyond x=1 or fully behind z=0.
    const float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    const GpuFrustum frustum = GpuFrustum::fromVulkanViewProjection(identity);
    assert(frustum.containsChunk(0,0,0));
    assert(!frustum.containsChunk(64,0,0));
    assert(!frustum.containsChunk(-64,0,0));
    assert(!frustum.containsChunk(0,0,-64));
    assert(!frustum.containsChunk(0,0,64));
    // Translated camera with an orthographic range [-64,+64] in X/Y,
    // and [0,128] in Z. CPU reference must match GPU-positive-vertex test.
    const float ortho[16] = {
        1.f/64,0,0,0, 0,1.f/64,0,0, 0,0,1.f/128,0, 0,0,0,1
    };
    const GpuFrustum wide = GpuFrustum::fromVulkanViewProjection(ortho);
    assert(wide.containsChunk(32,0,96));
    assert(wide.containsChunk(-64,0,0));
    assert(!wide.containsChunk(96,0,0));
    assert(!wide.containsChunk(0,0,-64));
    assert(!wide.containsChunk(0,0,160));
    try { GpuFrustum::fromVulkanViewProjection(nullptr); assert(false); }
    catch (const std::invalid_argument&) {}
    std::cout << "Vulkan world core + GPU frustum ABI checks passed\n";
}
