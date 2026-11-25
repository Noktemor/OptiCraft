#pragma once
#include "rendering/vulkan/VoxelChannelPages.h"
#include "rendering/vulkan/LightVerticalBounds.h"
#include <array>
#include <cstdint>
#include <span>
#include <vector>
#include <limits>
#include <map>
#include <algorithm>

namespace OptiCraft::Vulkan {
// Prototype closed-set lighting transaction. Slot/page ownership must be pinned
// externally until completion. Inputs must be authoritative seed light, not
// stale propagated light after an edit, unless rebuilt from current IDs.
// No writes to resident pages here.
struct ResidentLightBatch {
    static constexpr uint32_t missing=UINT32_MAX, words=8192, blockWords=16384;
    static constexpr uint32_t transparencyWords=2048, emissionWords=8192;
    // Same emission cap as Generation_Sky and CPU block-light seeds.
    static constexpr uint32_t emissionLevel(uint8_t e) noexcept {return std::min(uint32_t(e),14u);}
    static uint32_t packEmission(std::span<const uint8_t> emission,
                                 std::span<uint32_t> packed) noexcept {
        if(emission.size()!=65536||packed.size()!=emissionWords)return missing;
        uint32_t maximum=0;
        for(uint32_t w=0;w<emissionWords;++w){
            uint32_t value=0;
            for(uint32_t i=0;i<8;++i){const auto e=emissionLevel(emission[w*8+i]);
                maximum=std::max(maximum,e);value|=e<<(i*4);}
            packed[w]=value;
        }
        return maximum?maximum-1:0;
    }
    struct Job {
        uint32_t slot;
        std::array<uint32_t,6> neighbor; // compact job index, -X,+X,-Y,+Y,-Z,+Z
        uint32_t reserved=0;
    };
    struct Params {uint32_t jobCount, readBank, writeBank;}; // readBank=2: resident seeds; 3: rebuild seeds
    static_assert(sizeof(Job)==32 && sizeof(Params)==12);
    std::vector<Job> jobs;
    // Caller passes active modern stable-slot snapshots, with selected IDs
    // referring to pinned generations. Reject malformed selected links rather
    // than risk reading unrelated resident pages. Unselected links are closed.
    template<class Slot>
    bool compile(std::span<const Slot> slots,std::span<const uint32_t> selected,
                 uint64_t blockCapacity,uint64_t lightCapacity,uint64_t scratchCapacity) {
        jobs.clear();
        if(selected.empty() || selected.size()>65535 ||
           uint64_t(selected.size())*words*2>scratchCapacity) return false;
        std::vector<uint32_t> map(slots.size(),missing);
        std::vector<Job> next;
        std::map<std::array<int64_t,3>,uint32_t> coordinates;
        next.reserve(selected.size());
        for(uint32_t id:selected) {
            if(id>=slots.size() || map[id]!=missing) return false;
            const auto& s=slots[id]; const auto flags=uint32_t(s.origin[3]);
            if(!s.source[3] || !(flags&kVoxelAddressFlag)) return false;
            if(!(flags&kVoxelUniformBlock) && uint64_t(s.source[0])+blockWords>blockCapacity) return false;
            if(!(flags&kVoxelUniformLight) && uint64_t(uint32_t(s.neighborB[3]))+words>lightCapacity) return false;
            if(!coordinates.emplace(std::array<int64_t,3>{s.origin[0],s.origin[1],s.origin[2]},id).second) return false;
            map[id]=uint32_t(next.size());
            next.push_back({id,{missing,missing,missing,missing,missing,missing}});
        }
        const auto link=[](const Slot& s,unsigned f){return f<4?s.neighborA[f]:s.neighborB[f-4];};
        for(auto& j:next) {
            const auto& s=slots[j.slot];
            for(unsigned f=0;f<6;++f) {
                const int32_t n=link(s,f);
                std::array<int64_t,3> pos{s.origin[0],s.origin[1],s.origin[2]};
                pos[f/2u]+=(f&1u)?32:-32;
                const auto expected=coordinates.find(pos);
                if(expected!=coordinates.end() && (n<0 || uint32_t(n)!=expected->second)) return false;
                if(n==-1) continue;
                if(n<0 || uint32_t(n)>=slots.size()) return false;
                if(map[n]==missing) continue;
                const auto& other=slots[n];
                if(link(other,f^1u)!=int32_t(j.slot)) return false;
                for(unsigned a=0;a<3;++a) {
                    const int64_t delta=int64_t(other.origin[a])-s.origin[a];
                    if(delta!=(a==f/2u?((f&1u)?32:-32):0)) return false;
                }
                j.neighbor[f]=map[n];
            }
        }
        // A missing link must not silently close one direction of a selected edge.
        for(uint32_t i=0;i<next.size();++i) for(unsigned f=0;f<6;++f) {
            const auto n=next[i].neighbor[f];
            if(n!=missing && next[n].neighbor[f^1u]!=i) return false;
        }
        jobs.swap(next); return true;
    }
    // A block emitter has radius at most 13 voxels (15 in the legacy
    // certificate means radius 14), less than one 32-voxel chunk. Every
    // path contributing to a core voxel stays inside its one-chunk halo,
    // including edge/corner neighbours. Only core results are publishable.
    // Unknown/missing halo slots are NOT assumed air. Fail closed; callers
    // can use the existing CPU path at world/streaming boundaries.
    // Core IDs retain caller order and occupy the prefix of selected.
    template<class Slot>
    static bool selectCore(std::span<const Slot> slots,std::span<const uint32_t> core,
                           uint32_t limit,std::vector<uint32_t>& selected,LightVerticalBounds bounds={}) {
        selected.clear();
        if(core.empty() || core.size()>limit || limit>65535 || slots.size()>UINT32_MAX) return false;
        using Pos=std::array<int64_t,3>;
        const auto pos=[](const Slot& s){return Pos{s.origin[0],s.origin[1],s.origin[2]};};
        std::map<Pos,uint32_t> coordinates;
        for(uint32_t i=0;i<slots.size();++i) {
            const auto& s=slots[i];
            if(!s.source[3] || !(uint32_t(s.origin[3])&kVoxelAddressFlag)) continue;
            if(!coordinates.emplace(pos(s),i).second) return false;
        }
        std::vector<uint8_t> seen(slots.size(),0);
        std::vector<uint32_t> next;
        next.reserve(std::min<size_t>(limit,slots.size()));
        for(uint32_t id:core) {
            if(id>=slots.size() || seen[id] || !bounds.contains(int64_t(slots[id].origin[1])/32) || !slots[id].source[3] ||
               !(uint32_t(slots[id].origin[3])&kVoxelAddressFlag)) return false;
            seen[id]=1;next.push_back(id);
        }
        for(uint32_t id:core) {
            const auto p=pos(slots[id]);
            for(int x=-1;x<=1;++x)for(int y=-1;y<=1;++y)for(int z=-1;z<=1;++z) {
                if(!bounds.contains(p[1]/32+y))continue;
                const auto it=coordinates.find(Pos{p[0]+32*x,p[1]+32*y,p[2]+32*z});
                if(it==coordinates.end()) return false;
                const auto n=it->second;
                if(seen[n]) continue;
                if(next.size()>=limit) return false;
                seen[n]=1;next.push_back(n);
            }
        }
        selected.swap(next);return true;
    }
    // Exact certificate over a closed batch. Inputs are the same pinned
    // snapshots passed to compile(); result is one dense bank in job order.
    // With emission supplied, rebuild expected block seeds from IDs and use
    // only the existing sky nibble; otherwise require seed-only input.
    template<class Slot>
    bool matches(std::span<const Slot> slots,std::span<const uint32_t> blocks,
                 std::span<const uint32_t> seeds,std::span<const uint32_t> result,
                 std::span<const uint32_t> transparency,
                 std::span<const uint8_t> emission={}) const noexcept {
        if(jobs.empty() || result.size()!=jobs.size()*words || transparency.size()!=2048 || (!emission.empty() && emission.size()!=65536)) return false;
        for(const auto& j:jobs) {
            if(j.slot>=slots.size()) return false;
            const auto& s=slots[j.slot];const auto flags=uint32_t(s.origin[3]);
            if(!s.source[3] || !(flags&kVoxelAddressFlag)) return false;
            if(!(flags&kVoxelUniformBlock) && uint64_t(s.source[0])+blockWords>blocks.size()) return false;
            if(!(flags&kVoxelUniformLight) && uint64_t(uint32_t(s.neighborB[3]))+words>seeds.size()) return false;
            for(auto n:j.neighbor) if(n!=missing && n>=jobs.size()) return false;
        }
        const auto level=[&](uint32_t j,uint32_t i) {
            return j==missing?0u:(result[j*words+i/4]>>((i&3u)*8u))&15u;
        };
        for(uint32_t j=0;j<jobs.size();++j) {
            const auto& s=slots[jobs[j].slot];const auto flags=uint32_t(s.origin[3]);
            const auto& n=jobs[j].neighbor;
            for(uint32_t i=0;i<32768;++i) {
                const uint32_t bw=(flags&kVoxelUniformBlock)?s.source[0]:blocks[s.source[0]+i/2];
                const uint32_t id=(bw>>((i&1u)*16u))&65535u;
                const uint32_t sw=(flags&kVoxelUniformLight)?uint32_t(s.neighborB[3]):seeds[uint32_t(s.neighborB[3])+i/4];
                uint32_t seed=(sw>>((i&3u)*8u))&255u;
                if(!emission.empty()) seed=(seed&240u)|emissionLevel(emission[id]);
                uint32_t expected=seed;
                if((transparency[id/32u]>>(id&31u))&1u) {
                    const uint32_t x=i&31u,z=(i>>5u)&31u,y=i>>10u;
                    uint32_t v=level(x?j:n[0],x?i-1:i+31);
                    v=std::max(v,level(x!=31?j:n[1],x!=31?i+1:i-31));
                    v=std::max(v,level(y?j:n[2],y?i-1024:i+31744));
                    v=std::max(v,level(y!=31?j:n[3],y!=31?i+1024:i-31744));
                    v=std::max(v,level(z?j:n[4],z?i-32:i+992));
                    v=std::max(v,level(z!=31?j:n[5],z!=31?i+32:i-992));
                    expected=(seed&240u)|std::max(seed&15u,v?v-1:0u);
                }
                if(((result[j*words+i/4]>>((i&3u)*8u))&255u)!=expected) return false;
            }
        }
        return true;
    }
    static constexpr Params seedPass(uint32_t count,uint32_t passes) noexcept {
        return {count,3u,1u^(passes&1u)};
    }
    static constexpr Params generatedIteration(uint32_t count,uint32_t passes,unsigned pass) noexcept {
        const uint32_t read=seedPass(count,passes).writeBank^(pass&1u);
        return {count,read,read^1u};
    }
    static constexpr Params iteration(uint32_t count,unsigned pass) noexcept {
        return {count,pass==0?2u:((pass-1u)&1u),pass&1u};
    }
};
}
