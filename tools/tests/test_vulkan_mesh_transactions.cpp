#include "rendering/vulkan/GpuMeshTransactions.h"
#include <cassert>
#include <array>
#include <iostream>
#include <stdexcept>

using namespace OptiCraft::Vulkan;

int main(){
    GpuMeshTransactions q(16, 1024*8, 1024*32, 4);
    q.dirty(3,1);
    q.dirty(3,1); // merge duplicate, increment revision
    q.dirty(4,2);
    assert(q.startCount());
    assert(q.jobs().size()==2);
    assert(q.jobs()[0].slot==3);
    assert(q.jobs()[1].slot==4);
    std::array<uint32_t,2> opaque{8,4}, misc{2,3};
    assert(q.finishCount(opaque,misc));
    assert(q.phase()==GpuMeshTransactions::Phase::Emitting);
    assert(q.opaqueUsed() == (8+4)*8);
    assert(q.miscUsed() == (2+3)*32);
    auto changes=q.verifyEmit(opaque,misc);
    assert(changes[0].valid&&changes[1].valid);
    q.acceptPublish(changes);
    assert(q.stats().published==2);
    assert(q.opaqueUsed()==96);
    q.dirty(3,1); // rebuild candidate, previous range stays until publication
    assert(q.startCount());
    std::array<uint32_t,1> newOpaque{5},newMisc{1};
    assert(q.finishCount(newOpaque,newMisc));
    assert(q.opaqueUsed()==96+5*8);
    q.dirty(3,1); // changed again before emit completion
    auto invalid=q.verifyEmit(newOpaque,newMisc);
    assert(!invalid[0].valid);
    q.acceptPublish(invalid);
    assert(q.opaqueUsed()==96); // original remains
    assert(q.startCount());
    assert(q.finishCount(newOpaque,newMisc));
    auto good=q.verifyEmit(newOpaque,newMisc);
    q.acceptPublish(good);
    assert(q.opaqueUsed()==96+40); // replacement plus retired old
    q.completedFrame();q.completedFrame();
    assert(q.opaqueUsed()==136);
    q.completedFrame();
    assert(q.opaqueUsed()==72); // 40 + slot4 32
    q.dirty(4,2);
    assert(q.startCount());
    std::array<uint32_t,1> tooLarge{1000000},zero{0};
    assert(!q.finishCount(tooLarge,zero)); // preserve last published
    assert(q.stats().overflow==1);
    assert(q.opaqueUsed()==72);
    q.dirty(4,2);
    assert(q.startCount());
    assert(q.finishCount(newOpaque,newMisc));
    std::array<uint32_t,1> mismatch{6};
    auto failed=q.verifyEmit(mismatch,newMisc);
    assert(!failed[0].valid);
    q.acceptPublish(failed);
    assert(q.opaqueUsed()==72);
    q.evict(3,1);
    q.evict(4,999); // stale generation must not release data
    for(int i=0;i<3;++i)q.completedFrame();
    assert(q.opaqueUsed()==32);
    q.dirty(3,3); // reused slot with new generation
    assert(q.startCount());
    assert(q.finishCount(newOpaque,newMisc));
    q.evict(3,3); // immediately before GPU emit completion
    auto gone=q.verifyEmit(newOpaque,newMisc);
    assert(!gone[0].valid);
    q.acceptPublish(gone);
    assert(q.opaqueUsed()==32);
    q.evict(4,2);
    for(int i=0;i<3;++i)q.completedFrame();
    assert(q.opaqueUsed()==0 && q.miscUsed()==0);
    // OOM is not allowed to produce a partial opaque/misc pair.
    GpuMeshTransactions small(2,32,32,2);
    small.dirty(0,1);
    assert(small.startCount());
    std::array<uint32_t,1> four{4},two{2};
    assert(!small.finishCount(four,two));
    assert(small.opaqueUsed()==0 && small.miscUsed()==0);
    assert(small.stats().overflow==1);
    bool caught=false;
    try{q.dirty(100,1);}catch(const std::out_of_range&){caught=true;}
    assert(caught);
    std::cout<<"GPU mesh transactions: all checks passed\n";
}
