#include "rendering/vulkan/VulkanGameChunkFeed.h"
#include "world/chunks/Chunk_Manager.h"
#include "rendering/vulkan/SurfaceLod.h"
#include "rendering/vulkan/VulkanChunkStreamPlan.h"
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string_view>
using namespace OptiCraft::Vulkan;
// Only opaque lease identity is needed by the host-only feed fixture.
namespace OptiCraft::Vulkan {struct OverlaySnapshot {};}
int main(){
    const char* opt=std::getenv("OPTICRAFT_VULKAN_COMPACT_UPLOAD");
    const bool compact=!opt||std::string_view(opt)!="0";
    Chunk_Manager world;
    auto& solid=world.chunks[0];solid.blocks.ids={513};solid.lights.uniform=0xf3;
    auto& air=world.chunks[1];air.lights.uniform=0xf0;
    auto& mixed=world.chunks[2];mixed.blocks.ids={12,65000};
    mixed.blocks.dense.resize(kChunkVoxels);mixed.lights.dense.resize(kChunkVoxels);
    for(uint32_t i=0;i<kChunkVoxels;++i){mixed.blocks.dense[i]=mixed.blocks.ids[i%2];mixed.lights.dense[i]=uint8_t(i%256);}
    mixed.blocks.meta={0,128,0,0,0,201,32767,55};
    mixed.overlay_ref.ticket=123;mixed.overlay_version=mixed.version;
    VulkanGameChunkFeed feed;std::vector<LiveChunkPacket> packets;
    feed.setSink([&](auto&& p){packets=std::move(p);});
    std::array<uint8_t,65536> kinds{};kinds[513]=kinds[12]=kinds[65000]=1;feed.setMeshKinds(kinds);
    feed.acceptFrame({{{0,0,0}},{{1,0,0}},{{2,0,0}}},world);
    assert(packets.size()==3); // includes lit air halo
    for(size_t slot=0;slot<packets.size();++slot){
        auto& p=packets[slot];const auto& c=world.chunks[slot];
        assert(p.position.x==int(slot));
        assert(p.overlayReference.ticket==(slot==2?123u:0u));
        assert(p.blockWords.size()==(compact&&slot<2?1:kChunkBlockWords));
        assert(p.metaWords.size()==(compact&&slot<2?1:kChunkVoxels/4));
        assert(p.lightWords.size()==(compact&&slot<2?1:kChunkVoxels/4));
        assert(c.blocks.unpackCalls==unsigned(!compact||slot==2));
        assert(c.blocks.metaCalls==unsigned(!compact));
        assert(c.lights.unpackCalls==unsigned(!compact||slot==2));
        for(uint32_t i=0;i<kChunkVoxels;++i){
            const auto b=p.blockWords[p.blockWords.size()==1?0:i/2];
            const auto m=p.metaWords[p.metaWords.size()==1?0:i/4];
            const auto l=p.lightWords[p.lightWords.size()==1?0:i/4];
            assert(((b>>((i&1u)*16))&65535u)==(slot<2?c.blocks.ids[0]:c.blocks.dense[i]));
            assert(((m>>((i&3u)*8))&255u)==(slot==2?(i==0?201u:i==32767?55u:0u):0u));
            assert(((l>>((i&3u)*8))&255u)==(slot<2?c.lights.uniform:c.lights.dense[i]));
        }
    }
    const char* lodOpt=std::getenv("OPTICRAFT_VULKAN_LOD_CONTROL");
    const bool control=!lodOpt||std::string_view(lodOpt)!="0";
    const char* lightOpt=std::getenv("OPTICRAFT_VULKAN_LIGHT_CONTROL");
    const bool lightControl=!lightOpt||std::string_view(lightOpt)!="0";
    // Real feed path: a pure opaque/air palette can advance to LOD2, but
    // a mixed misc palette and a fully solid palette keep the exact topology.
    mixed.blocks.ids={12,0};for(auto& id:mixed.blocks.dense)if(id==65000)id=0;++mixed.version;
    feed.setPriorityCenter({-14,0,0});
    feed.acceptFrame({{{0,0,0}},{{1,0,0}},{{2,0,0}}},world);
    assert(packets.size()==2 && packets[0].position.x==0 && packets[1].position.x==2);
    assert((packets[0].meshKinds&kLodFlags)==kSurfaceLodFlag);
    assert((packets[1].meshKinds&kLodFlags)==kLod2Flags);
    assert(!packets[1].overlayReference.ticket);
    assert(!packets[1].lodOnly); // actual block change must carry full data
    assert(packets[0].lodOnly==control);
    const auto blockCalls=mixed.blocks.unpackCalls,metaCalls=mixed.blocks.metaCalls,lightCalls=mixed.lights.unpackCalls;
    feed.setPriorityCenter({-18,0,0});
    feed.acceptFrame({{{0,0,0}},{{1,0,0}},{{2,0,0}}},world);
    assert(packets.size()==1 && packets[0].position.x==2 && (packets[0].meshKinds&kLodFlags)==kLodFlags);
    assert(packets[0].lodOnly==control);
    if(control){
        assert(packets[0].blockWords.empty() && packets[0].metaWords.empty() && packets[0].lightWords.empty());
        assert(mixed.blocks.unpackCalls==blockCalls && mixed.blocks.metaCalls==metaCalls && mixed.lights.unpackCalls==lightCalls);
    }
    feed.setPriorityCenter({-16,0,0});
    feed.acceptFrame({{{0,0,0}},{{1,0,0}},{{2,0,0}}},world);
    assert(packets[0].meshKinds&kFarGeometryLodFlag);
    feed.setPriorityCenter({-15,0,0});
    feed.acceptFrame({{{0,0,0}},{{1,0,0}},{{2,0,0}}},world);
    assert(packets.size()==1 && packets[0].position.x==2 && (packets[0].meshKinds&kLodFlags)==kLod2Flags);
    feed.setPriorityCenter({-10,0,0});
    feed.acceptFrame({{{0,0,0}},{{1,0,0}},{{2,0,0}}},world);
    assert(packets[0].meshKinds&kGeometryLodFlag); // no new packets, hysteresis
    feed.setPriorityCenter({-9,0,0});
    feed.acceptFrame({{{0,0,0}},{{1,0,0}},{{2,0,0}}},world);
    assert(packets.size()==1 && packets[0].position.x==2 && (packets[0].meshKinds&kLodFlags)==kSurfaceLodFlag);
    mixed.blocks.ids={12,65000};for(auto& id:mixed.blocks.dense)if(id==0)id=65000;kinds[65000]=2;feed.setMeshKinds(kinds);++mixed.version;
    feed.setPriorityCenter({-15,0,0});
    feed.acceptFrame({{{0,0,0}},{{1,0,0}},{{2,0,0}}},world);
    assert(packets.size()==1 && packets[0].position.x==2 && (packets[0].meshKinds&kLodFlags)==kSurfaceLodFlag);
    // A renderer-side eviction can invalidate a delta source. The entire
    // callback is rejected atomically, and feed retries full snapshots.
    if(control){
        VulkanGameChunkFeed retry;VulkanChunkStreamPlan plan(8,16);
        retry.setMeshKinds(kinds);
        retry.setSink([&](auto&& p){plan.submit(std::move(p));},[&](auto k){plan.evict(k);});
        const std::vector<Renderable_Chunk> visible{{{0,0,0}},{{1,0,0}},{{2,0,0}}};
        retry.acceptFrame(visible,world);assert(plan.take(8).uploads.size()==3);
        plan.evict({0,0,0,0});assert(plan.take(8).releases.size()==1);
        retry.setPriorityCenter({-14,0,0});retry.acceptFrame(visible,world);
        assert(retry.trackedCount()==3 && plan.pendingCount()==0);
        retry.acceptFrame(visible,world);auto recovered=plan.take(8);
        assert(recovered.uploads.size()==3 && retry.trackedCount()==3);
        assert(std::all_of(recovered.uploads.begin(),recovered.uploads.end(),[](const auto& u){return !u.packet.lodOnly;}));
        assert(plan.lookup({0,0,0,0}));
        // Another miss followed by a selection change must still evict
        // previously acknowledged slots, not leak them outside the budget.
        plan.evict({0,0,0,0});assert(plan.take(8).releases.size()==1);
        retry.setPriorityCenter({0,0,0});retry.acceptFrame(visible,world);
        assert(plan.pendingCount()==0 && retry.trackedCount()==3);
        retry.setPriorityCenter({1,0,0});retry.acceptFrame({{{0,0,0}},{{1,0,0}}},world);
        auto reduced=plan.take(8);assert(reduced.releases.size()==1 && reduced.releases[0].key.x==2);
        assert(plan.residentCount()==2 && !plan.lookup({0,2,0,0}));
    }
    // Stage57: actual light revision update does not unpack blocks/meta.
    {
        VulkanGameChunkFeed f;VulkanChunkStreamPlan plan(8,16);
        std::vector<LiveChunkPacket> output;
        f.setMeshKinds(kinds);f.setSink([&](auto&& p){output=p;plan.submit(std::move(p));},[&](auto k){plan.evict(k);});
        const std::vector<Renderable_Chunk> visible{{{2,0,0}}};
        f.acceptFrame(visible,world);assert(plan.take(8).uploads.size()==1);
        const auto b=mixed.blocks.unpackCalls,m=mixed.blocks.metaCalls,l=mixed.lights.unpackCalls;
        ++mixed.dirty;mixed.lights.dense[16]=42;f.setPriorityCenter({1,0,0});
        f.acceptFrame(visible,world);auto batch=plan.take(8);
        assert(batch.uploads.size()==1 && output.size()==1 && output[0].lightOnly==lightControl);
        assert(output[0].lightWords[4]%256==42 && mixed.lights.unpackCalls==l+1);
        if(lightControl){
            assert(output[0].blockWords.empty() && output[0].metaWords.empty() && !output[0].overlayReference.ticket);
            assert(mixed.blocks.unpackCalls==b && mixed.blocks.metaCalls==m);
            plan.evict({0,2,0,0});plan.take(8);
            ++mixed.dirty;f.setPriorityCenter({2,0,0});f.acceptFrame(visible,world);
            assert(f.trackedCount()==1 && plan.pendingCount()==0);
            f.acceptFrame(visible,world);batch=plan.take(8);
            assert(batch.uploads.size()==1 && !batch.uploads[0].packet.lightOnly);
        }
    }
    // Stage58: an available lease is acquired before unpacking blocks.
    // The actual feed still packs light; uniform metadata stays inline.
    {
        VulkanGameChunkFeed f;VulkanChunkStreamPlan plan(8,16);
        std::vector<LiveChunkPacket> output;
        auto lease=std::make_shared<OverlaySnapshot>();
        mixed.overlay_ref={lease,601};mixed.overlay_version=mixed.version;
        mixed.blocks.meta.clear();
        const char* sourceOpt=std::getenv("OPTICRAFT_VULKAN_GPU_SOURCE_PACKETS");
        const bool sourceEnabled=!sourceOpt||std::string_view(sourceOpt)!="0";
        f.setMeshKinds(kinds);f.setSink([&](auto&& p){output=p;plan.submit(std::move(p));},[&](auto k){plan.evict(k);},
            [&](const OverlayReference& ref){return ref.ticket==601?ref.source.lock():std::shared_ptr<OverlaySnapshot>{};});
        const auto b=mixed.blocks.unpackCalls,m=mixed.blocks.metaCalls;
        const std::vector<Renderable_Chunk> visible{{{2,0,0}}};
        f.acceptFrame(visible,world);assert(output.size()==1);
        assert(bool(output[0].gpuSourceLease)==sourceEnabled);
        if(sourceEnabled){
            assert(output[0].blockWords.empty());
            assert(output[0].metaWords.size()==(compact?1:0));
            assert(mixed.blocks.unpackCalls==b && mixed.blocks.metaCalls==m);
        }else assert(mixed.blocks.unpackCalls==b+1);
        assert(plan.take(8).uploads.size()==1);
        // A changed revision loses the reference and uses a normal snapshot.
        ++mixed.version;f.setPriorityCenter({1,0,0});f.acceptFrame(visible,world);
        assert(output.size()==1 && !output[0].gpuSourceLease && output[0].blockWords.size()==kChunkBlockWords);
        assert(plan.take(8).uploads.size()==1);
        // A still-matching but expired reference must also serialize on CPU.
        mixed.overlay_version=mixed.version;lease.reset();++mixed.dirty;
        f.setSink([&](auto&& p){output=std::move(p);},{},
            [&](const OverlayReference& ref){return ref.source.lock();});f.acceptFrame(visible,world);
        assert(output.size()==1 && !output[0].gpuSourceLease && output[0].blockWords.size()==kChunkBlockWords);
    }
    // Stage53: actual feed -> stream planner; no source version is forged
    // on camera policy changes, and all CPU light bytes remain available.
    {
        Chunk_Manager far;
        const ChunkKey core{0,8,0,0};
        std::vector<Renderable_Chunk> visible;
        for(int x=7;x<=9;++x)for(int y=-1;y<=1;++y)for(int z=-1;z<=1;++z){
            auto& c=far.extra[{0,x,y,z}];c.blocks.ids={513};c.lights.uniform=0xf0;
            visible.push_back({{x,y,z}});
        }
        auto& c=far.extra[core];c.lights.dense.resize(kChunkVoxels,0xf1);
        c.lights.dense[17]=0xfa;
        VulkanGameChunkFeed pilot;VulkanChunkStreamPlan plan(64,128);
        std::vector<LiveChunkPacket> output;
        pilot.setSink([&](auto&& p){output=p;plan.submit(std::move(p));},[&](auto k){plan.evict(k);});
        pilot.setMeshKinds(kinds);pilot.setTrackedBudget(64);pilot.setResidentLightPilot(true);
        auto frame=[&](int x,size_t budget=64){output.clear();pilot.setPriorityCenter({x,0,0});pilot.acceptFrame(visible,far,0,budget);return plan.take(64);};
        auto first=frame(0);assert(first.uploads.size()==27);
        auto resident=plan.lookup(core).value();assert(resident.gpuBlockLightOwned);
        assert(std::count_if(output.begin(),output.end(),[](const auto& p){return p.gpuBlockLightOwned;})==1);
        const auto original=*std::find_if(output.begin(),output.end(),[&](const auto& p){return p.position==core;});
        assert(original.lightWords[4]==0xf1f1faf1u && original.blocksVersion==1 && original.meshVersion==1);
        assert(original.lightOwnershipRevision!=0);
        frame(1);assert(plan.lookup(core)->gpuBlockLightOwned); // hysteresis
        frame(3,1);assert(output.size()==1 && output.front().position==core); // handback first
        auto back=output.front();assert(!back.gpuBlockLightOwned && back.blocksVersion==original.blocksVersion && back.meshVersion==original.meshVersion);
        assert(back.lightOwnershipRevision>original.lightOwnershipRevision && back.lightWords==original.lightWords);
        assert(back.lightOnly==lightControl);
        assert(!plan.lookup(core)->gpuBlockLightOwned);
        // A late old ownership decision cannot undo handback, even with a
        // larger topology revision or newer source data from that old policy.
        auto late=original;late.lodRevision=back.lodRevision+100;late.blocksVersion=2;
        plan.submit({late});assert(plan.take(64).empty());
        // A newer ownership decision is allowed without source changes.
        frame(0);assert(plan.lookup(core)->gpuBlockLightOwned);
        // Missing corner (not merely a face) prevents continued ownership.
        const auto missing=visible.back();visible.pop_back();
        frame(1,1);assert(!plan.lookup(core)->gpuBlockLightOwned);
        visible.push_back(missing);frame(0);assert(plan.lookup(core)->gpuBlockLightOwned);
        pilot.setResidentLightPilot(false);frame(0);assert(!plan.lookup(core)->gpuBlockLightOwned);
        // Old policy cannot replace a queued new decision either.
        auto current=plan.lookup(core).value();auto promote=back;promote.gpuBlockLightOwned=true;
        promote.lightOwnershipRevision=current.lightOwnershipRevision+1;
        promote.lodRevision=promote.lightOwnershipRevision;
        auto stale=original;stale.blocksVersion=3;
        plan.submit({promote});plan.submit({stale});auto batch=plan.take(64);
        assert(batch.uploads.size()==1 && batch.uploads[0].uploadVoxels && plan.lookup(core)->gpuBlockLightOwned);
        // Camera policy does not modify CPU-side data.
        assert(c.version==1 && c.dirty==1 && c.lights.dense[17]==0xfa);
    }
    // Actual producer feed: edits bypass zero ordinary packet budget.
    {
        Chunk_Manager editWorld;
        for(auto& c:editWorld.chunks)c.blocks.ids={12};
        VulkanGameChunkFeed editFeed;VulkanChunkStreamPlan plan(16,32);
        editFeed.setMeshKinds(kinds);
        editFeed.setSink([&](auto&& p){plan.submit(std::move(p));},[&](auto k){plan.evict(k);});
        std::vector<Renderable_Chunk> visible{{{0,0,0}},{{1,0,0}},{{2,0,0}}};
        editFeed.acceptFrame(visible,editWorld,0,1);assert(plan.take(16).uploads.size()==1);
        auto& far=editWorld.chunks[2];far.interactive=++Chunk::interactiveEvents;++far.version;++far.dirty;
        auto& near=editWorld.chunks[1];near.interactive=++Chunk::interactiveEvents;++near.dirty;
        editFeed.acceptFrame(visible,editWorld,0,0);
        auto immediate=plan.take(16,{}, {},true);assert(immediate.uploads.size()==2);
        for(const auto& u:immediate.uploads)assert(u.packet.interactive && u.packet.position.x!=0);
        editFeed.acceptFrame(visible,editWorld,0,0);assert(plan.pendingCount()==0);
        far.interactive=++Chunk::interactiveEvents;++far.dirty;++far.version;
        editFeed.acceptFrame(visible,editWorld,0,0);
        assert(plan.take(16,{}, {},true).uploads[0].packet.position.x==2);
    }
    std::cout<<"Actual feed source + mock chunks: "<<(compact?"compact":"dense diagnostic")<<" payloads PASS\n";
}
