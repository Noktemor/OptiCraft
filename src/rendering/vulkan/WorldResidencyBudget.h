#pragma once
#include "world/chunks/Gpu_World_Distances.h"
#include <algorithm>
#include <cstdint>
#include <limits>

namespace OptiCraft::Vulkan {
// Admission policy for the existing fixed slot table and growing voxel arenas.
// Byte budget is for total tracked GPU allocation, including retired buffers
// if the caller can account for them. It does not query driver VRAM itself.
class WorldResidencyBudget {
public:
    struct Limits {uint32_t slots=512,queue=1024,reserveSlots=16;uint64_t bytes=256ull<<20;};
    struct Usage {uint32_t resident=0,pending=0;uint64_t allocatedBytes=0,pendingBytes=0;};
    struct Decision {uint32_t admissions=0;bool pressure=false,hardLimit=false;uint64_t requestedSlots=0;};
    explicit WorldResidencyBudget(Limits limits):limits_(limits){limits_.reserveSlots=std::min(limits_.reserveSlots,limits_.slots);}
    Decision update(const Usage& u,const Gpu_World_Streaming::Window& w,uint32_t requested,uint64_t bytesPerAdmission) noexcept {
        const uint64_t slots=uint64_t(u.resident)+u.pending;
        const uint64_t used=saturatingAdd(u.allocatedBytes,u.pendingBytes);
        const uint64_t softSlots=limits_.slots-limits_.reserveSlots;
        const uint64_t softBytes=limits_.bytes-limits_.bytes/8;
        const bool hard=slots>=limits_.slots||used>=limits_.bytes||u.pending>=limits_.queue;
        if(hard||slots>=softSlots||used>=softBytes)pressure_=true;
        else if(slots<=softSlots-softSlots/8&&used<=softBytes-softBytes/8&&u.pending<=limits_.queue/2)pressure_=false;
        uint64_t n=0;
        if(!pressure_&&!hard&&bytesPerAdmission){
            n=std::min({uint64_t(requested),softSlots-slots,uint64_t(limits_.queue-u.pending),(softBytes-used)/bytesPerAdmission});
        }
        return {uint32_t(n),pressure_,hard,Gpu_World_Streaming::requestedSlots(w)};
    }
    // Out-of-window candidates only, preserving CPU guard and visible world.
    // Capacity shortfall in the visible window is surfaced rather than silently
    // churning the same chunks. Eviction is not retirement authorization.
    static bool evictionCandidate(const Gpu_World_Streaming::Window& w,int x,int y,int z) noexcept {
        return Gpu_World_Streaming::validWindow(w)&&!Gpu_World_Streaming::visible(w,x,y,z);
    }
    // Startup capacity planning: caller supplies measured worst-case per-slot
    // memory and a hard device/descriptor cap. No runtime slot ID migration.
    static uint32_t plannedCapacity(const Gpu_World_Streaming::Window& w,uint64_t bytes,
                                   uint64_t bytesPerSlot,uint32_t deviceCap) noexcept {
        if(!bytesPerSlot)return 0;
        return uint32_t(std::min({Gpu_World_Streaming::requestedSlots(w),bytes/bytesPerSlot,uint64_t(deviceCap)}));
    }
private:
    static uint64_t saturatingAdd(uint64_t a,uint64_t b) noexcept{return b>UINT64_MAX-a?UINT64_MAX:a+b;}
    Limits limits_;bool pressure_=false;
};
}
