#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

namespace OptiCraft::Vulkan {
// Vulkan guarantees at most 16 memory heaps. Heap flags, not memory type flags,
// classify UMA/BAR memory without counting a host-visible device-local heap twice.
struct DeviceMemoryUsage {
    struct Heap {uint64_t tracked=0,budget=0,used=0;bool local=false;};
    std::array<Heap,16> heaps{};
    uint32_t count=0,allocations=0;
    bool driverBudget=false;
    uint64_t total()const noexcept {uint64_t n=0;for(auto h:heaps)n+=h.tracked;return n;}
    uint64_t local()const noexcept {uint64_t n=0;for(auto h:heaps)if(h.local)n+=h.tracked;return n;}
    uint64_t localUsed()const noexcept {uint64_t n=0;for(auto h:heaps)if(h.local)n+=h.used;return n;}
    uint64_t localBudget()const noexcept {uint64_t n=0;for(auto h:heaps)if(h.local)n+=h.budget;return n;}
};
// Driver usage is an estimate sampled outside admission. Charge growth since
// sampling without crediting frees until the next query; never undercount our
// live explicit allocations even if the driver estimate lags.
inline DeviceMemoryUsage withDriverBudget(DeviceMemoryUsage current,const DeviceMemoryUsage& sample,
        const std::array<uint64_t,16>& budgets,const std::array<uint64_t,16>& usage) noexcept {
    current.driverBudget=true;
    for(uint32_t i=0;i<current.count&&i<16;++i){
        auto& h=current.heaps[i];
        h.budget=std::min(h.budget,budgets[i]);
        const uint64_t growth=h.tracked>sample.heaps[i].tracked?h.tracked-sample.heaps[i].tracked:0;
        h.used=std::max(h.tracked,growth>UINT64_MAX-usage[i]?UINT64_MAX:usage[i]+growth);
    }
    return current;
}
// Admission only: no eviction of visible chunks or cancellation of work in flight.
// Pending bytes are charged conservatively against each active heap because future
// buffer requirements may select different memory types. The physical-heap fallback
// is not a driver/OS free-memory estimate.
class DeviceMemoryAdmission {
public:
    bool allow(const DeviceMemoryUsage& s,uint64_t pending,uint64_t next) noexcept {
        const uint64_t needed=next>UINT64_MAX-pending?UINT64_MAX:pending+next;
        bool blocked=false,recover=true;
        uint32_t largest=UINT32_MAX;
        for(uint32_t i=0;i<std::min<uint32_t>(s.count,16);++i)
            if(s.heaps[i].local && (largest==UINT32_MAX||s.heaps[i].budget>s.heaps[largest].budget))largest=i;
        for(uint32_t i=0;i<std::min<uint32_t>(s.count,16);++i){
            const auto& h=s.heaps[i];
            if(!h.tracked && i!=largest)continue;
            const uint64_t soft=h.budget-h.budget/8;
            const uint64_t free=h.used>=soft?0:soft-h.used;
            const uint64_t margin=std::min<uint64_t>(8ull<<20,h.budget/32);
            if(needed>free)blocked=true;
            if(needed>free || free-needed<margin)recover=false;
        }
        if(blocked)pressure_=true;else if(recover)pressure_=false;
        return !pressure_;
    }
    bool pressure()const noexcept{return pressure_;}
private: bool pressure_=false;
};
}
