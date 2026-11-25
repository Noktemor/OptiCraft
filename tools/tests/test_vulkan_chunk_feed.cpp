#include "rendering/vulkan/VulkanGameChunkFeed.h"
#include "world/chunks/Chunk_Manager.h"
#include "rendering/vulkan/SurfaceLod.h"
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string_view>
using namespace OptiCraft::Vulkan;
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
    VulkanGameChunkFeed feed;std::vector<LiveChunkPacket> packets;
    feed.setSink([&](auto&& p){packets=std::move(p);});
    std::array<uint8_t,65536> kinds{};kinds[513]=kinds[12]=kinds[65000]=1;feed.setMeshKinds(kinds);
    feed.acceptFrame({{{0,0,0}},{{1,0,0}},{{2,0,0}}},world);
    assert(packets.size()==3); // includes lit air halo
    for(size_t slot=0;slot<packets.size();++slot){
        auto& p=packets[slot];const auto& c=world.chunks[slot];
        assert(p.position.x==int(slot));
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
    // Real feed path: a pure opaque/air palette can advance to LOD2, but
    // a mixed misc palette and a fully solid palette keep the exact topology.
    mixed.blocks.ids={12,0};for(auto& id:mixed.blocks.dense)if(id==65000)id=0;++mixed.version;
    feed.setPriorityCenter({-14,0,0});
    feed.acceptFrame({{{0,0,0}},{{1,0,0}},{{2,0,0}}},world);
    assert(packets.size()==2 && packets[0].position.x==0 && packets[1].position.x==2);
    assert((packets[0].meshKinds&kLodFlags)==kSurfaceLodFlag);
    assert((packets[1].meshKinds&kLodFlags)==kLod2Flags);
    feed.setPriorityCenter({-18,0,0});
    feed.acceptFrame({{{0,0,0}},{{1,0,0}},{{2,0,0}}},world);
    assert(packets.size()==1 && packets[0].position.x==2 && (packets[0].meshKinds&kLodFlags)==kLodFlags);
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
    std::cout<<"Actual feed source + mock chunks: "<<(compact?"compact":"dense diagnostic")<<" payloads PASS\n";
}
