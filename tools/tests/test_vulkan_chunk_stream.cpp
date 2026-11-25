#include "rendering/vulkan/VulkanChunkStreamPlan.h"
#include <cassert>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <thread>
#include "rendering/vulkan/MeshDispatchRuns.h"
#include "rendering/vulkan/MeshResidency.h"
#include "rendering/vulkan/SurfaceLod.h"
using namespace OptiCraft::Vulkan;

static LiveChunkPacket packet(int x, uint64_t instance, uint64_t version) {
    LiveChunkPacket p;
    p.position = {0,x,0,0};
    p.chunkInstance=instance;
    p.blocksVersion=version;
    p.meshVersion=version;
    p.blockWords.resize(kChunkBlockWords,uint32_t(version));
    p.metaWords.resize(kChunkVoxels/4,uint32_t(version));
    p.lightWords.resize(kChunkVoxels/4,uint32_t(version));
    return p;
}
int main() {
    {
        VulkanChunkStreamPlan editStream(16,64);
        editStream.submit({packet(0,1,1),packet(1,1,1),packet(2,1,1)});
        editStream.take(16);
        for(int i=3;i<12;++i)editStream.submit({packet(i,1,1)});
        auto background=packet(0,1,2);background.priority=0;
        auto edit=packet(2,1,2);edit.priority=100;edit.interactive=true;
        editStream.submit({background,edit});
        auto newerEdit=packet(2,1,3);newerEdit.priority=100;
        editStream.submit({newerEdit});
        auto urgent=editStream.take(1,{}, {},true);
        assert(urgent.uploads.size()==1 && urgent.uploads[0].packet.position.x==2);
        assert(urgent.uploads[0].packet.blocksVersion==3 && urgent.uploads[0].packet.interactive);
        assert(std::binary_search(urgent.interactiveRemeshSlots.begin(),urgent.interactiveRemeshSlots.end(),2u));
        assert(std::binary_search(urgent.interactiveRemeshSlots.begin(),urgent.interactiveRemeshSlots.end(),1u));
        assert(editStream.take(8,{}, {},true).empty());
        assert(editStream.pendingCount()==10);
        assert(editStream.take(1).uploads[0].packet.position.x==0);
        auto retry=packet(2,1,4);retry.interactive=true;editStream.submit({retry});
        assert(editStream.take(1,[](auto,auto const&,bool){return false;},{},true).empty());
        assert(editStream.take(1,{}, {},true).uploads[0].packet.position.x==2);
    }
    VulkanChunkStreamPlan stream(3,4);
    std::vector<LiveChunkPacket> initial;
    initial.push_back(packet(0,1,1));
    initial.push_back(packet(1,2,1));
    stream.submit(std::move(initial));
    assert(stream.pendingCount()==2);
    auto first = stream.take(1);
    assert(first.uploads.size()==1 && stream.pendingCount()==1);
    assert(stream.residentCount()==1);
    assert(first.uploads[0].slot==0 && first.uploads[0].packet.position.x==0);
    auto second=stream.take(8);
    assert(second.uploads.size()==1 && second.uploads[0].slot==1);
    assert(second.remeshSlots.size()==2); // x=0 must rebuild border face
    assert(second.remeshSlots[0]==0 && second.remeshSlots[1]==1);
    assert(stream.residentCount()==2 && stream.pendingCount()==0);
    // An old update cannot roll back a live chunk or cause a needless upload.
    stream.submit({packet(0,1,1)});
    assert(stream.take(4).empty());
    stream.submit({packet(0,1,2),packet(0,1,3)});
    auto latest=stream.take(1);
    assert(latest.uploads.size()==1 && latest.uploads[0].packet.blocksVersion==3);
    assert(!latest.uploads[0].newResident); // revision update does not rebuild live-slot list
    // Synthetic word values change only the low half; +X boundary (odd cell) stays identical.
    assert(latest.remeshSlots.size()==1 && latest.remeshSlots[0]==0);
    stream.evict({0,1,0,0});
    auto evicted=stream.take(0);
    assert(evicted.releases.size()==1 && evicted.releases[0].slot==1);
    assert(evicted.remeshSlots.size()==1 && evicted.remeshSlots[0]==0);
    assert(!stream.lookup({0,1,0,0}).has_value());
    stream.submit({packet(5,4,1)});
    auto recycled=stream.take(1);
    assert(recycled.uploads.size()==1 && recycled.uploads[0].slot==1);
    assert(recycled.uploads[0].newResident);
    assert(recycled.uploads[0].generation!=evicted.releases[0].generation);
    // Cancel queued upload before its GPU submission.
    stream.submit({packet(6,5,1)});
    stream.evict({0,6,0,0});
    assert(stream.take(16).empty());
    // Replacement of same key by a new instance must not reuse stale generation.
    auto old=stream.lookup({0,5,0,0}).value();
    stream.submit({packet(5,6,1)});
    auto replaced=stream.take(1);
    assert(replaced.uploads.size()==1 && replaced.uploads[0].slot==old.slot);
    assert(replaced.uploads[0].generation!=old.generation);
    // Queue full: reject entire callback without accepting a partial packet.
    try {stream.submit({packet(7,10,1),packet(8,11,1),packet(9,12,1),
                         packet(10,13,1),packet(11,14,1)}); assert(false);}
    catch(const std::overflow_error&) {}
    assert(stream.pendingCount()==0);
    try {auto malformed=packet(3,2,1); malformed.metaWords.pop_back();
         stream.submit({std::move(malformed)}); assert(false);}
    catch(const std::invalid_argument&) {}
    try {stream.submit({packet(70000000,2,1)}); assert(false);}
    catch(const std::out_of_range&) {}
    // Bounded residency, newest pending snapshot survives without being dropped.
    stream.submit({packet(2,20,1),packet(3,21,1)});
    auto limited=stream.take(8);
    assert(limited.uploads.size()==1);
    assert(stream.residentCount()==3 && stream.pendingCount()==1);
    stream.evict({0,0,0,0});
    auto resumed=stream.take(8);
    assert(resumed.releases.size()==1 && resumed.uploads.size()==1);
    assert(stream.residentCount()==3 && stream.pendingCount()==0);
    // Multiple producers: coalescing and locking, never duplicate GPU uploads.
    VulkanChunkStreamPlan threaded(16,32);
    std::vector<std::thread> workers;
    for(int i=0;i<6;i++) workers.emplace_back([&,i] {
        for(int j=0;j<16;j++) threaded.submit({packet(i,1,uint64_t(i*16+j+1))});
    });
    for(auto& t:workers)t.join();
    auto concurrent=threaded.take(16);
    assert(concurrent.uploads.size()==6);
    for(const auto& v:concurrent.uploads)
        assert(v.packet.blocksVersion==uint64_t(v.packet.position.x*16+16));
    VulkanChunkStreamPlan fast(16,32);
    auto center=packet(0,1,1);center.meshKinds=5;center.priority=0;
    auto make=[&](int x,int y,int z,uint64_t priority){auto v=packet(x,1,1);v.position.y=y;v.position.z=z;v.priority=priority;v.meshKinds=5;return v;};
    fast.submit({make(-1,0,0,1),center,make(1,0,0,1),make(0,-1,0,1),make(0,1,0,1),make(0,0,-1,1),make(0,0,1,1)});
    auto near=fast.take(1);
    assert(near.uploads[0].packet.position==center.position);
    const auto slot=near.uploads[0].slot;
    assert(!fast.fullyOccluded(slot));
    assert(fast.take(8).uploads.size()==6);
    assert(fast.fullyOccluded(slot));
    auto changed=make(1,0,0,1);changed.blocksVersion=2;changed.meshKinds=3;
    fast.submit({changed});
    assert(fast.take(0).empty()); // frozen sources while Count/Emit is in flight
    assert(fast.fullyOccluded(slot));
    const auto delta=fast.take(1);
    assert(delta.uploads.size()==1 && !fast.fullyOccluded(slot));
    assert(std::find(delta.remeshSlots.begin(),delta.remeshSlots.end(),slot)!=delta.remeshSlots.end());
    // Change one interior voxel, then each face separately, including light/meta.
    VulkanChunkStreamPlan borders(8,16);
    auto centerPacket=packet(0,1,1);
    std::fill(centerPacket.blockWords.begin(),centerPacket.blockWords.end(),0u);
    std::fill(centerPacket.metaWords.begin(),centerPacket.metaWords.end(),0u);
    std::fill(centerPacket.lightWords.begin(),centerPacket.lightWords.end(),0u);
    borders.submit({centerPacket});assert(borders.take(1).uploads.size()==1);
    const std::array<ChunkKey,6> sides{{{0,-1,0,0},{0,1,0,0},{0,0,-1,0},{0,0,1,0},{0,0,0,-1},{0,0,0,1}}};
    for(auto key:sides){auto n=centerPacket;n.position=key;borders.submit({n});}
    assert(borders.take(8).uploads.size()==6);
    auto change=[&](uint32_t x,uint32_t y,uint32_t z,uint32_t kind){
        uint32_t i=y*1024+z*32+x;
        if(kind==0)centerPacket.blockWords[i/2]^=1u<<((i%2)*16);
        else if(kind==1)centerPacket.metaWords[i/4]^=1u<<((i%4)*8);
        else centerPacket.lightWords[i/4]^=1u<<((i%4)*8);
        ++centerPacket.meshVersion;
        borders.submit({centerPacket});return borders.take(1);
    };
    assert(change(15,15,15,0).remeshSlots==std::vector<uint32_t>{0});
    for(uint32_t side=0;side<6;++side){
        std::array<uint32_t,3> pos{15,15,15};pos[side/2]=(side&1u)?31:0;
        for(uint32_t kind=0;kind<3;++kind){
            const auto batch=change(pos[0],pos[1],pos[2],kind);
            assert(batch.remeshSlots.size()==2 && batch.remeshSlots[0]==0);
            assert(batch.remeshSlots[1]==borders.lookup(sides[side])->slot);
        }
    }
    const auto corner=change(0,0,0,2);
    assert(corner.remeshSlots.size()==4);
    // Dispatch masks preserve the original count index even with holes.
    const std::array<uint8_t,8> kinds{5,5,3,0,2,1,5,5};
    const auto opaque=meshDispatchRuns(8,kinds,false,1);
    assert(opaque.size()==6 && opaque[0].groupsX==6 && opaque[2].first==2 && opaque[2].groupsX==192 && opaque[3].first==5);
    const auto misc=meshDispatchRuns(8,kinds,true,8);
    assert(misc.size()==2 && misc[0].first==2 && misc[1].first==4 && misc[1].groupsX==512);
    const auto grouped=meshDispatchRuns(8,kinds,false,8);
    assert(grouped.size()==4 && grouped[0].count==2 && grouped.back().count==2);
    assert(meshDispatchRuns(2,{},false,8)[0].groupsX==192);
    assert(meshDispatchRuns(2,{},true,8)[0].groupsX==512);
    const std::array<uint8_t,6> cached{uint8_t(1|kCachedOpaqueMesh),uint8_t(5|kCachedOpaqueMesh),0,1,5,2};
    const auto cachedRuns=meshDispatchRuns(6,cached,false,8);
    assert(cachedRuns.size()==3 && cachedRuns[0].groupsX==8 && cachedRuns[0].count==2);
    assert(cachedRuns[1].first==3 && cachedRuns[1].groupsX==192 && cachedRuns[2].groupsX==6);
    // A bounded partial sort must preserve resident priority, distance and ties,
    // even if the queue contains obsolete snapshots and the pool is full.
    VulkanChunkStreamPlan priority(3,32);
    priority.submit({packet(0,1,1),packet(1,1,1)});priority.take(2);
    auto existingUpdate=packet(1,1,2);existingUpdate.priority=999;
    auto candidateA=packet(9,1,1);candidateA.priority=2;
    auto candidateB=packet(8,1,1);candidateB.priority=2;
    auto candidateC=packet(7,1,1);candidateC.priority=3;
    priority.submit({candidateA,candidateB,candidateC,packet(0,1,1),existingUpdate});
    auto chosen=priority.take(2);
    assert(chosen.uploads.size()==2 && chosen.uploads[0].packet.position.x==1 && chosen.uploads[1].packet.position.x==8);
    assert(priority.pendingCount()==2 && priority.take(8).empty());
    priority.evict({0,0,0,0});assert(priority.take(1).uploads[0].packet.position.x==9);
    priority.evict({0,1,0,0});assert(priority.take(1).uploads[0].packet.position.x==7);
    const std::unordered_set<ChunkKey,ChunkKeyHash> terrain{{0,0,0,0},{1,4,4,4}};
    for(auto key:sides)assert(needsAirHalo(key,terrain));
    assert(!needsAirHalo({0,1,1,0},terrain));
    assert(!needsAirHalo({0,0,3,0},terrain));
    assert(!needsAirHalo({0,4,4,5},terrain));
    assert(needsAirHalo({1,4,4,5},terrain));
    assert(!needsAirHalo({0,INT32_MAX,0,0},terrain));
    assert(!surfaceLod(99.0,false)&&surfaceLod(100.0,false));
    assert(surfaceLod(64.0,true)&&!surfaceLod(63.9,true));
    assert(!geometryLod(195.9,false)&&geometryLod(196.0,false));
    assert(geometryLod(144.0,true)&&!geometryLod(143.9,true));
    assert(selectSurfaceLod(196,0,true,true,true,true)==kLod2Flags);
    assert(selectSurfaceLod(150,kLod2Flags,true,true,true,true)==kLod2Flags);
    assert(selectSurfaceLod(143,kLod2Flags,true,true,true,true)==kSurfaceLodFlag);
    assert(selectSurfaceLod(200,0,true,false,true,true)==kSurfaceLodFlag);
    assert(selectSurfaceLod(200,0,true,true,true,false)==kSurfaceLodFlag);
    assert(selectSurfaceLod(200,0,true,true,false,true)==0);
    assert(selectSurfaceLod(399.9,0,true,true,true,true)==kLod2Flags);
    assert(selectSurfaceLod(400,0,true,true,true,true)==kLodFlags);
    assert(selectSurfaceLod(324,kLodFlags,true,true,true,true)==kLodFlags);
    assert(selectSurfaceLod(323.9,kLodFlags,true,true,true,true)==kLod2Flags);
    assert(selectSurfaceLod(500,0,true,false,true,true)==kSurfaceLodFlag);
    VulkanChunkStreamPlan lod(2,4);
    auto a=packet(0,1,1),b=packet(1,2,1);
    lod.submit({a,b});lod.take(2);
    a.lodRevision=1;a.meshKinds|=kSurfaceLodFlag;
    lod.submit({a});auto lodOnly=lod.take(1);assert(lodOnly.remeshSlots==std::vector<uint32_t>{0});
    assert(lodOnly.uploads.size()==1 && !lodOnly.uploads[0].uploadVoxels);
    a.lodRevision=0;a.meshKinds&=~kSurfaceLodFlag;
    lod.submit({a});assert(lod.take(1).empty());
    a.lodRevision=2;lod.submit({a});auto lodBack=lod.take(1);
    assert(lodBack.remeshSlots==std::vector<uint32_t>{0} && !lodBack.uploads[0].uploadVoxels);
    ++a.lodRevision;a.meshKinds|=kLod2Flags;lod.submit({a});auto coarseLod=lod.take(1);
    assert(coarseLod.remeshSlots==std::vector<uint32_t>{0} && !coarseLod.uploads[0].uploadVoxels);
    assert(lod.slotInfo(0)->meshKinds&kGeometryLodFlag);
    ++a.lodRevision;a.meshKinds|=kFarGeometryLodFlag;lod.submit({a});auto farLod=lod.take(1);
    assert(farLod.remeshSlots==std::vector<uint32_t>{0} && !farLod.uploads[0].uploadVoxels);
    assert(lod.slotInfo(0)->meshKinds&kFarGeometryLodFlag);
    ++a.lodRevision;a.meshKinds&=~kFarGeometryLodFlag;lod.submit({a});auto farBack=lod.take(1);
    assert(farBack.remeshSlots==std::vector<uint32_t>{0} && !farBack.uploads[0].uploadVoxels);
    ++a.lodRevision;a.meshKinds&=~kGeometryLodFlag;lod.submit({a});auto fineAgain=lod.take(1);
    assert(fineAgain.remeshSlots==std::vector<uint32_t>{0} && !fineAgain.uploads[0].uploadVoxels);
    ++a.meshVersion;++a.lodRevision;lod.submit({a});assert(lod.take(1).uploads[0].uploadVoxels);
    // Stage56: LOD control packets need an existing source or a pending full
    // payload. No boundary scan, page admission or neighbor dirtying is needed.
    {
        VulkanChunkStreamPlan deltaPlan(4,8);
        auto full=packet(0,11,1);full.overlayReference.ticket=42;
        auto neighbor=packet(1,12,1);
        auto delta=full;delta.lodOnly=true;delta.overlayReference={};
        delta.blockWords.clear();delta.metaWords.clear();delta.lightWords.clear();
        delta.lodRevision=1;delta.meshKinds|=kSurfaceLodFlag;
        auto reject=[&](std::vector<LiveChunkPacket> packets){
            const auto pending=deltaPlan.pendingCount();
            try{deltaPlan.submit(std::move(packets));assert(false);}catch(const std::invalid_argument&){}
            assert(deltaPlan.pendingCount()==pending);
        };
        reject({neighbor,delta}); // atomic rejection with valid unrelated full packet
        deltaPlan.submit({full,neighbor});deltaPlan.submit({delta});
        auto first=deltaPlan.take(4);assert(first.uploads.size()==2);
        const auto& merged=first.uploads[0];
        assert(merged.uploadVoxels && !merged.packet.lodOnly && merged.packet.blockWords==full.blockWords);
        assert(merged.packet.metaWords==full.metaWords && merged.packet.lightWords==full.lightWords);
        assert(merged.packet.overlayReference.ticket==42 && merged.packet.lodRevision==1);
        const auto boundary=deltaPlan.boundaryPayloadBytes();
        delta.lodRevision=2;delta.meshKinds|=kGeometryLodFlag;
        deltaPlan.submit({delta});++delta.lodRevision;delta.meshKinds|=kFarGeometryLodFlag;
        deltaPlan.submit({delta});
        auto update=deltaPlan.take(4,[](auto,const auto&,bool){assert(false);return false;});
        assert(update.uploads.size()==1 && update.uploads[0].packet.lodOnly && !update.uploads[0].uploadVoxels);
        assert(update.remeshSlots==std::vector<uint32_t>{0} && deltaPlan.boundaryPayloadBytes()==boundary);
        assert(deltaPlan.lookup(full.position)->lodRevision==3);
        auto stale=delta;--stale.lodRevision;deltaPlan.submit({stale});assert(deltaPlan.take(4).empty());
        auto wrong=delta;++wrong.blocksVersion;reject({wrong});
        wrong=delta;++wrong.meshVersion;reject({wrong});
        wrong=delta;++wrong.chunkInstance;reject({wrong});
        wrong=delta;wrong.gpuBlockLightOwned=true;reject({wrong});
        wrong=delta;++wrong.lightOwnershipRevision;reject({wrong});
        wrong=delta;wrong.meshKinds^=2;reject({wrong});
        wrong=delta;wrong.blockWords={0};reject({wrong});
        reject({full,delta});reject({delta,delta});
        // A later source upload supersedes a queued control packet.
        ++delta.lodRevision;deltaPlan.submit({delta});
        ++full.blocksVersion;++full.meshVersion;full.lodRevision=5;
        deltaPlan.submit({full});assert(deltaPlan.take(4).uploads[0].uploadVoxels);
        reject({delta}); // old source must not replace it
        deltaPlan.evict(full.position);delta=full;delta.lodOnly=true;delta.overlayReference={};
        delta.blockWords.clear();delta.metaWords.clear();delta.lightWords.clear();++delta.lodRevision;
        reject({delta});assert(deltaPlan.take(4).releases.size()==1);reject({delta});
        deltaPlan.submit({full});deltaPlan.submit({delta});assert(deltaPlan.take(4).uploads[0].uploadVoxels);
    }
    // Stage57: independent light payload, full snapshot coalescing, exact
    // boundary retention, LOD composition and atomic rejection of missing source.
    {
        VulkanChunkStreamPlan plan(8,16);
        auto full=packet(0,11,1);full.overlayReference.ticket=42;
        auto neighbor=packet(1,12,1);
        full.blockWords={0x12341234};full.metaWords={0x56565656};full.lightWords={0x78787878};
        auto light=full;light.lightOnly=true;light.overlayReference={};
        light.blockWords.clear();light.metaWords.clear();light.lightWords={0x89898989};
        ++light.meshVersion;++light.lodRevision;
        auto reject=[&](std::vector<LiveChunkPacket> packets){
            auto pending=plan.pendingCount();try{plan.submit(std::move(packets));assert(false);}catch(const std::invalid_argument&){}
            assert(plan.pendingCount()==pending);
        };
        reject({neighbor,light});
        plan.submit({full,neighbor});plan.submit({light});
        auto first=plan.take(8);assert(first.uploads.size()==2);
        const auto& merged=first.uploads[0].packet;
        assert(!merged.lightOnly && merged.blockWords==full.blockWords && merged.metaWords==full.metaWords);
        assert(merged.lightWords==light.lightWords && merged.overlayReference.ticket==42);
        auto boundary=plan.boundaryPayloadBytes();
        auto lod=light;lod.lightOnly=false;lod.lodOnly=true;lod.lightWords.clear();
        ++lod.lodRevision;lod.meshKinds|=kSurfaceLodFlag;
        plan.submit({lod});
        light.meshKinds=lod.meshKinds;light.lodRevision=lod.lodRevision+1;
        ++light.meshVersion;light.lightWords.assign(kChunkVoxels/4,0x89898989);
        // A single changed interior light word must not dirty the neighbor.
        light.lightWords[16*256+16*8+4]=0x12345678;
        plan.submit({light});
        lod=light;lod.lodOnly=true;lod.lightOnly=false;lod.lightWords.clear();++lod.lodRevision;
        plan.submit({lod});
        auto refused=plan.take(8,[](auto,const auto& p,bool){assert(p.lightOnly);return false;});
        assert(refused.empty() && plan.pendingCount()==1);
        auto update=plan.take(8);assert(update.uploads.size()==1 && update.uploads[0].packet.lightOnly);
        assert(update.uploads[0].packet.lodRevision==lod.lodRevision);
        assert(update.remeshSlots==std::vector<uint32_t>{0} && plan.boundaryPayloadBytes()==boundary);
        // Modify one +X face; following full payload with the same boundary
        // must not dirty the neighbor again (block/meta boundary preserved).
        light.lodRevision=lod.lodRevision+1;++light.meshVersion;
        light.lightWords[(16*1024+16*32+31)/4]^=0x01000000u;
        plan.submit({light});assert((plan.take(8).remeshSlots==std::vector<uint32_t>{0,1}));
        full.lightWords=light.lightWords;full.meshVersion=light.meshVersion+1;
        full.lodRevision=light.lodRevision+1;full.meshKinds=light.meshKinds;
        plan.submit({full});assert(plan.take(8).remeshSlots==std::vector<uint32_t>{0});
        auto wrong=light;++wrong.blocksVersion;reject({wrong});wrong=light;++wrong.chunkInstance;reject({wrong});
        wrong=light;wrong.blockWords={0};reject({wrong});wrong=light;wrong.lodOnly=true;reject({wrong});
        reject({light,light});plan.submit({light});assert(plan.take(8).empty());
        plan.evict(full.position);reject({light});assert(plan.take(8).releases.size()==1);reject({light});
    }
    VulkanChunkStreamPlan compact(8,16);
    auto c=packet(0,1,1);
    c.blockWords={0x1234abcd};c.metaWords={0x12345678};c.lightWords={0xFEDCBA98};
    auto n=c;n.position.x=1;
    compact.submit({c,n});assert(compact.take(2).uploads.size()==2);
    auto dense=c;
    dense.blockWords.assign(kChunkBlockWords,c.blockWords[0]);
    dense.metaWords.assign(kChunkVoxels/4,c.metaWords[0]);
    dense.lightWords.assign(kChunkVoxels/4,c.lightWords[0]);
    dense.lodRevision=1;dense.meshKinds|=kSurfaceLodFlag;
    compact.submit({dense});auto denseLod=compact.take(1);
    assert(denseLod.remeshSlots==std::vector<uint32_t>{0} && !denseLod.uploads[0].uploadVoxels);
    c.lodRevision=2;compact.submit({c});auto compactLod=compact.take(1);
    assert(compactLod.remeshSlots==std::vector<uint32_t>{0} && !compactLod.uploads[0].uploadVoxels);
    c.lightWords={0xFFFFFFFF};++c.meshVersion;
    compact.submit({c});auto changedCompact=compact.take(1);
    assert(changedCompact.remeshSlots==std::vector<uint32_t>({0,1}));
    for(size_t invalidSize:{0u,2u,8191u}){
        auto invalid=c;invalid.metaWords.resize(invalidSize);
        try {compact.submit({invalid});assert(false);}catch(const std::invalid_argument&){}
    }
    // Exact dense/compact boundary transitions and actual payload residency.
    VulkanChunkStreamPlan boundaryPool(2,4);
    auto uniform=packet(0,1,1);uniform.blockWords={0x12341234};uniform.metaWords={0};uniform.lightWords={0xF0F0F0F0};
    auto neighbor=uniform;neighbor.position.x=1;
    boundaryPool.submit({uniform,neighbor});boundaryPool.take(2);
    assert(boundaryPool.boundaryPayloadBytes()==8);
    auto varied=uniform;varied.blockWords.assign(kChunkBlockWords,0x12341234);
    varied.blockWords[(15*1024+15*32+31)/2]^=0x10000u;++varied.blocksVersion;
    boundaryPool.submit({varied});assert(boundaryPool.take(1).remeshSlots==std::vector<uint32_t>({0,1}));
    assert(boundaryPool.boundaryPayloadBytes()==24576+4);
    uniform.blocksVersion=3;boundaryPool.submit({uniform});
    assert(boundaryPool.take(1).remeshSlots==std::vector<uint32_t>({0,1}));
    assert(boundaryPool.boundaryPayloadBytes()==8);
    boundaryPool.evict(neighbor.position);boundaryPool.take(0);assert(boundaryPool.boundaryPayloadBytes()==4);
    std::cout << "Vulkan chunk stream queue, exact borders, dispatch and air halo tests passed\n";
}
