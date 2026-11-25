#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>
namespace OptiCraft::Vulkan {
inline constexpr uint8_t kCachedOpaqueMesh=0x80u;
inline constexpr uint8_t kCoarseOpaqueMesh=0x40u; // CPU dispatch mask, not the slot ABI
inline constexpr uint32_t kCoarseCacheFlag=0x80000000u;
inline constexpr uint32_t kCoarseWordsPerJob=4096u;
inline constexpr uint32_t kPlaneCompactFlag=0x40000000u;
inline constexpr uint32_t kPlaneScanGroups=48u; // four consecutive planes per group
inline constexpr uint32_t kPlaneWordsPerJob=3u+192u+192u*32u;
inline constexpr bool compactPlaneKind(uint8_t k){return (k&(1u|4u|kCoarseOpaqueMesh|kCachedOpaqueMesh))==(1u|kCoarseOpaqueMesh);}
inline constexpr uint64_t planeArgsOffset(uint32_t jobs,uint32_t cap,uint32_t job){
    return 4ull*(uint64_t(jobs)*(1ull+2ull*cap+kCoarseWordsPerJob)+3ull*job);
}
struct MeshDispatchRun {uint32_t first,count,groupsX;};
// Empty kinds = legacy/probe all jobs, unknown palette. Zero = inactive.
inline std::vector<MeshDispatchRun> meshDispatchRuns(uint32_t jobs,std::span<const uint8_t> kinds,
                                                    bool misc,uint32_t maxY){
    std::vector<MeshDispatchRun> runs;
    if(!jobs)return runs;
    if(!maxY || (!kinds.empty() && kinds.size()!=jobs))
        throw std::invalid_argument("Invalid mesh dispatch job mask");
    const auto groups=[&](uint32_t i)->uint32_t {
        if(kinds.empty())return misc?512u:192u;
        const uint8_t k=kinds[i];
        if(misc)return (k&2u)?512u:0u;
        if((k&1u) && (k&kCachedOpaqueMesh))return 8u;
        return (k&1u)?((k&4u)?6u:192u):0u;
    };
    for(uint32_t i=0;i<jobs;){
        const uint32_t x=groups(i);
        if(!x){++i;continue;}
        uint32_t n=1;
        while(n<maxY && n<jobs-i && groups(i+n)==x)++n;
        runs.push_back({i,n,x});i+=n;
    }
    return runs;
}
} // namespace OptiCraft::Vulkan
