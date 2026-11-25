#include "rendering/vulkan/GpuMeshTransactions.h"
#include "rendering/vulkan/MeshWorkBudget.h"
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
    // Cheap jobs may be grouped; mixed/dense work is bounded independently of slots.
    GpuMeshTransactions weighted(32,1024,1024,16,16);
    for(uint32_t i=0;i<12;++i)weighted.dirty(i,1,1);
    weighted.dirty(12,1,12);
    assert(weighted.startCount() && weighted.jobs().size()==12 && weighted.batchWork(false)==12);
    assert(weighted.pendingCount()==1);
    std::array<uint32_t,12> zeros{};
    assert(weighted.finishCount(zeros,zeros));
    auto emptyDraws=weighted.verifyEmit(zeros,zeros);weighted.acceptPublish(emptyDraws);
    assert(weighted.startCount() && weighted.jobs().size()==1 && weighted.batchWork(false)==12);
    assert(weighted.finishCount(newOpaque,newMisc));
    weighted.clearEmpty(12,1); // same generation becomes empty, then nonempty again before old emit completes
    weighted.dirty(12,1,8);
    auto obsolete=weighted.verifyEmit(newOpaque,newMisc);
    assert(!obsolete[0].valid);weighted.acceptPublish(obsolete);
    assert(weighted.startCount() && weighted.jobs().size()==1);
    assert(weighted.finishCount(newOpaque,newMisc));
    auto latestDraw=weighted.verifyEmit(newOpaque,newMisc);weighted.acceptPublish(latestDraw);
    assert(latestDraw[0].valid);
    weighted.clearEmpty(12,1);
    for(int i=0;i<3;++i)weighted.completedFrame();
    assert(weighted.opaqueUsed()==0 && weighted.miscUsed()==0);
    // Pending slot token prevents canceled jobs jumping ahead after same-slot reuse.
    weighted.dirty(1,2);weighted.clearEmpty(1,2);weighted.dirty(2,2);weighted.dirty(1,2);
    assert(weighted.startCount() && weighted.jobs()[0].slot==2 && weighted.jobs()[1].slot==1);
    MeshWorkBudget budget;budget.reset(16,32);
    budget.sample(12.0,16);assert(budget.budget()==4);
    for(int i=0;i<3;++i)budget.sample(1.0,4);
    assert(budget.budget()==5);
    for(int i=0;i<30;++i)budget.sample(0.5,budget.budget());
    assert(budget.budget()==32);
    budget.sample(1.0,1);assert(budget.budget()==32); // underfilled sample must not grow
    MeshWorkBudget phased;phased.reset(16,32);
    for(int i=0;i<20;++i)phased.sample(0.2,16,false);
    assert(phased.budget()==16); // cheap cached Emit never grows Count budget
    phased.sample(12.0,16,false);assert(phased.budget()==4); // slow Emit still shrinks
    for(int i=0;i<3;++i){phased.sample(0.7,4,true);phased.sample(0.2,4,false);}
    assert(phased.budget()==5);
    // Allocation failures are retried only when completed fences free old meshes.
    GpuMeshTransactions retry(3,32,64,1);
    retry.dirty(0,1);assert(retry.pendingCount()==1 && retry.startCount());
    assert(retry.pendingCount()==0 && retry.finishCount(four,zero));
    auto allocated=retry.verifyEmit(four,zero);retry.acceptPublish(allocated);
    retry.dirty(1,1);assert(retry.startCount());
    assert(!retry.finishCount(four,zero) && retry.stats().allocationFailures==1);
    assert(!retry.hasPending());
    retry.evict(0,1);
    retry.completedFrame();retry.completedFrame();assert(!retry.hasPending());
    retry.completedFrame();assert(retry.hasPending() && retry.stats().allocationRetries==1);
    assert(retry.startCount() && retry.jobs()[0].slot==1 && retry.finishCount(four,zero));
    auto retried=retry.verifyEmit(four,zero);retry.acceptPublish(retried);
    assert(retried[0].valid && retry.stats().published==2);
    retry.dirty(2,1);assert(retry.startCount());assert(!retry.finishCount(four,zero));
    retry.evict(2,1);retry.evict(1,1);
    for(int i=0;i<3;++i)retry.completedFrame();
    assert(!retry.hasPending() && retry.stats().allocationRetries==1);
    // Two concurrent batches share one arena; no duplicate slot in flight.
    GpuMeshTransactions parallel(8,1024,1024,2,16,2);
    parallel.dirty(0,1);parallel.dirty(1,1);parallel.dirty(2,1);
    parallel.selectBatch(0);assert(parallel.startCount());
    parallel.dirty(0,1); // in-flight revision becomes stale, queued for retry
    parallel.selectBatch(1);assert(parallel.startCount());
    assert(parallel.jobs().size()==1 && parallel.jobs()[0].slot==2);
    assert(parallel.inFlightCount()==3 && parallel.pendingCount()==1);
    assert(parallel.finishCount(newOpaque,newMisc));
    const auto secondRange=parallel.jobs()[0].opaqueCandidate;
    parallel.selectBatch(0);assert(parallel.finishCount(opaque,misc));
    assert(!parallel.jobs()[0].eligible && parallel.jobs()[1].eligible);
    assert(parallel.jobs()[1].opaqueCandidate.offset!=secondRange.offset);
    parallel.evict(1,1);parallel.dirty(1,2); // recycle before emit/publication
    assert(!parallel.isCurrent(parallel.jobs()[1]));
    auto firstChanges=parallel.verifyEmit(opaque,misc);
    assert(!firstChanges[0].valid && !firstChanges[1].valid);
    parallel.acceptPublish(firstChanges);
    assert(parallel.startCount() && parallel.jobs().size()==2);
    assert(parallel.jobs()[0].slot==0 && parallel.jobs()[1].generation==2);
    assert(parallel.finishCount(opaque,misc));
    parallel.selectBatch(1);auto other=parallel.verifyEmit(newOpaque,newMisc);
    assert(other[0].valid);parallel.acceptPublish(other);
    parallel.selectBatch(0);auto replacement=parallel.verifyEmit(opaque,misc);
    assert(replacement[0].valid && replacement[1].valid);parallel.acceptPublish(replacement);
    assert(parallel.stats().published==3 && parallel.opaqueUsed()==136);
    assert(parallel.inFlightCount()==0 && parallel.pendingCount()==0);
    parallel.evict(0,1);parallel.evict(1,2);parallel.evict(2,1);
    for(int k=0;k<3;++k)parallel.completedFrame();
    assert(parallel.opaqueUsed()==0 && parallel.miscUsed()==0);
    // Slab ownership is transferred only after fence, with exact alignment gaps
    // and tails returned. Malformed partitions must leave ownership unchanged.
    RangeArena arena(512);
    auto whole=arena.allocate(256,16);assert(whole && arena.used()==256);
    const std::array<BufferRange,2> overlap{{{16,24},{32,16}}};
    assert(!arena.partition(*whole,overlap) && arena.owns(*whole));
    const std::array<BufferRange,2> retain{{{16,24},{48,8}}};
    assert(arena.partition(*whole,retain) && arena.used()==32);
    assert(!arena.owns(*whole) && arena.owns(retain[0]) && arena.owns(retain[1]));
    assert(arena.release(retain[0]) && arena.release(retain[1]));
    assert(arena.used()==0 && arena.freeFragments()==1);
    // Two fast lanes reserve from the same arena. Odd sizes are aligned, empty
    // candidates consume zero bytes, cache overflow uses exact legacy fallback.
    using Draw=GpuMeshTransactions::FastDraw;
    GpuMeshTransactions fast(8,1024,1024,3,24,2);
    for(uint32_t k=0;k<5;++k)fast.dirty(k,1);
    assert(fast.startCount());auto slabA=fast.reserveFastOpaque(9);
    assert(slabA.offset==0 && slabA.size==240);
    fast.selectBatch(1);assert(fast.startCount());auto slabB=fast.reserveFastOpaque(9);
    assert(slabB.offset==240 && slabB.size==160 && fast.opaqueUsed()==400);
    const std::array<uint32_t,2> fastBCounts{3,0},z2{0,0};
    const std::array<Draw,2> fastBDraws{{{6,3,0,30},{6,0,0,34}}};
    fast.finishFastOpaque(fastBCounts,fastBDraws);
    assert(fast.opaqueUsed()==264);
    auto bDraws=fast.verifyEmit(fastBCounts,z2);assert(bDraws[0].valid && bDraws[1].valid);
    fast.acceptPublish(bDraws);
    fast.selectBatch(0);
    fast.dirty(2,1); // stale candidate after GPU dispatch, still occupies prefix
    const std::array<uint32_t,3> fastACounts{5,10,3},z3{0,0,0};
    const std::array<Draw,3> fastADraws{{{6,5,0,0},{0,0,0,0},{6,3,0,6}}};
    fast.finishFastOpaque(fastACounts,fastADraws);
    assert(fast.opaqueUsed()==64 && fast.pendingCount()==2);
    auto aDraws=fast.verifyEmit(fastACounts,z3);
    assert(aDraws[0].valid && !aDraws[1].valid && !aDraws[2].valid);
    fast.acceptPublish(aDraws);
    assert(fast.stats().fastJobs==3 && fast.stats().fastFallbacks==1);
    assert(fast.startCount() && fast.jobs().size()==2 && fast.jobs()[1].forceLegacy);
    assert(!fast.reserveFastOpaque(9)); // overflow revision cannot loop on fast path
    assert(fast.finishCount(opaque,z2));
    auto fallback=fast.verifyEmit(opaque,z2);fast.acceptPublish(fallback);
    // Corrupt draw metadata rejects the entire slab (no publication/alias).
    fast.dirty(0,1);assert(fast.startCount());auto corrupt=fast.reserveFastOpaque(9);assert(corrupt);
    const std::array<Draw,1> badDraw{{{6,5,0,uint32_t(corrupt.offset/8+1)}}};
    fast.finishFastOpaque(newOpaque,badDraw);
    auto badFast=fast.verifyEmit(newOpaque,zero);assert(!badFast[0].valid);fast.acceptPublish(badFast);
    assert(fast.pendingCount()==1 && fast.stats().fastFallbacks==2);
    assert(fast.startCount() && !fast.reserveFastOpaque(9));
    assert(fast.finishCount(newOpaque,zero));auto repaired=fast.verifyEmit(newOpaque,zero);fast.acceptPublish(repaired);
    for(uint32_t k=0;k<5;++k)fast.evict(k,1);
    for(int k=0;k<3;++k)fast.completedFrame();
    assert(fast.opaqueUsed()==0);
    // Reservation pressure falls back without consuming or invalidating a job.
    GpuMeshTransactions limited(2,32,64,1);
    limited.dirty(0,1);assert(limited.startCount() && !limited.reserveFastOpaque(9));
    assert(limited.stats().fastReservationMisses==1 && limited.finishCount(four,zero));
    auto exactLimited=limited.verifyEmit(four,zero);limited.acceptPublish(exactLimited);
    assert(limited.opaqueUsed()==32);
    GpuMeshTransactions canceled(2,256,64,1);
    canceled.dirty(0,1);assert(canceled.startCount() && canceled.reserveFastOpaque(9));
    canceled.cancelInFlight();assert(canceled.opaqueUsed()==0);
    // Mixed fast path: both geometry classes publish atomically, with fixed
    // misc strides compacted by releasing unused tails after the fence.
    GpuMeshTransactions mixed(8,2048,4096,3,24,2);
    for(uint32_t k=0;k<5;++k)mixed.dirty(k,1);
    assert(mixed.startCount());auto mo=mixed.reserveFastOpaque(8,5);auto mm=mixed.fastMiscSlab();
    assert(mo.size==192 && mm.size==480);
    mixed.selectBatch(1);assert(mixed.startCount());auto mo2=mixed.reserveFastOpaque(8,5);auto mm2=mixed.fastMiscSlab();
    assert(mo2.offset>=mo.offset+mo.size && mm2.offset>=mm.offset+mm.size);
    const std::array<uint32_t,2> oc2{0,3},mc2{5,0};
    const std::array<Draw,2> dr2{{{6,0,0,uint32_t(mo2.offset/8)},{6,3,0,uint32_t(mo2.offset/8)}}};
    mixed.finishFastOpaque(oc2,dr2,mc2);
    auto result2=mixed.verifyEmit(oc2,mc2);
    assert(result2[0].valid && result2[1].valid && result2[0].miscBase==mm2.offset/32);
    mixed.acceptPublish(result2);
    mixed.selectBatch(0);
    const std::array<uint32_t,3> oc3{4,9,2},mc3{6,2,3};
    const std::array<Draw,3> dr3{{{6,4,0,0},{0,0,0,0},{6,2,0,4}}};
    mixed.finishFastOpaque(oc3,dr3,mc3);
    auto result3=mixed.verifyEmit(oc3,mc3);
    assert(!result3[0].valid && !result3[1].valid && result3[2].valid);
    assert(result3[2].miscBase==10 && result3[2].miscCount==3);
    mixed.acceptPublish(result3);
    assert(mixed.opaqueUsed()==40 && mixed.miscUsed()==256);
    assert(mixed.stats().fastMixedJobs==3 && mixed.stats().fastMiscOverflow==1);
    assert(mixed.startCount() && !mixed.reserveFastOpaque(8,5));
    mixed.cancelInFlight();
    for(uint32_t k=0;k<5;++k)mixed.evict(k,1);
    for(int k=0;k<3;++k)mixed.completedFrame();
    assert(mixed.opaqueUsed()==0 && mixed.miscUsed()==0);
    GpuMeshTransactions rollback(2,256,64,1);
    rollback.dirty(0,1);assert(rollback.startCount() && !rollback.reserveFastOpaque(8,5));
    assert(rollback.opaqueUsed()==0 && rollback.miscUsed()==0);
    assert(rollback.reserveFastOpaque(8,2));rollback.cancelInFlight();
    assert(rollback.opaqueUsed()==0 && rollback.miscUsed()==0);
    // A mixed candidate that becomes stale between validation and publication
    // releases both new ranges, leaving the previous resident mesh untouched.
    GpuMeshTransactions staleMixed(2,1024,1024,1);
    staleMixed.dirty(0,1);assert(staleMixed.startCount() && staleMixed.finishCount(four,four));
    auto oldPair=staleMixed.verifyEmit(four,four);staleMixed.acceptPublish(oldPair);
    staleMixed.dirty(0,1);assert(staleMixed.startCount());auto newer=staleMixed.reserveFastOpaque(8,5);
    const std::array<Draw,1> newerDraw{{{6,4,0,uint32_t(newer.offset/8)}}};
    staleMixed.finishFastOpaque(four,newerDraw,four);staleMixed.dirty(0,1);
    auto stalePair=staleMixed.verifyEmit(four,four);assert(!stalePair[0].valid);staleMixed.acceptPublish(stalePair);
    assert(staleMixed.opaqueUsed()==32 && staleMixed.miscUsed()==128 && staleMixed.pendingCount()==1);
    std::cout<<"GPU mesh transactions: all checks passed\n";
}
