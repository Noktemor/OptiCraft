#include "rendering/vulkan/VulkanChunkStreamPlan.h"
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <thread>
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
    assert(latest.remeshSlots.size()==2);
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
    std::cout << "Vulkan chunk stream queue tests passed\n";
}
