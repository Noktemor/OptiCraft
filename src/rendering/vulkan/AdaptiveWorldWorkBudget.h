#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstddef>

namespace OptiCraft::Vulkan {
// Owner-thread scheduling model. Measurements are completed GPU timestamps,
// not CPU frame time. No fence waits; caller supplies each sample once.
class AdaptiveWorldWorkBudget {
public:
    enum Lane : size_t {Generation,Meshing,BlockLight,SkyLight,Transfers,Count};
    using Counts=std::array<uint32_t,Count>;
    using Costs=std::array<double,Count>;
    struct Sample {uint64_t serial=0;double frameMs=0;Counts work{};Costs ms{};};
    explicit AdaptiveWorldWorkBudget(double targetMs=16.67) noexcept:
        target_(std::isfinite(targetMs)?std::clamp(targetMs,2.0,100.0):16.67),quota_(target_*0.20){}
    bool observe(const Sample& s) noexcept {
        if(!s.serial||s.serial<=last_||!std::isfinite(s.frameMs)||s.frameMs<=0)return false;
        for(size_t i=0;i<Count;++i)if(s.work[i]&&(!std::isfinite(s.ms[i])||s.ms[i]<=0))return false;
        last_=s.serial;
        for(size_t i=0;i<Count;++i)if(s.work[i]){
            const double c=std::clamp(s.ms[i]/s.work[i],0.005,100.0);
            cost_[i]=seen_[i]?cost_[i]*0.75+c*0.25:c;seen_[i]=true;
        }
        if(s.frameMs>target_){quota_=std::max(target_*0.025,quota_*0.75);fast_=0;}
        else if(s.frameMs<target_*0.80){if(++fast_>=3){quota_=std::min(target_*0.45,quota_+target_*0.025);fast_=0;}}
        else fast_=0;
        return true;
    }
    // Bounded rotating allocation. One indivisible unit can exceed the quota
    // to guarantee progress, but only when nothing else was granted.
    Counts allocate(const Counts& pending,const Counts& caps) noexcept {
        Counts out{};double left=quota_;size_t total=0;
        constexpr size_t maxPerFrame=256;
        const auto first=cursor_;
        for(size_t step=0;step<maxPerFrame;++step){
            bool granted=false;
            for(size_t j=0;j<Count;++j){const size_t i=(first+j+step)%Count;
                if(out[i]>=std::min(pending[i],caps[i])||cost_[i]>left)continue;
                ++out[i];left-=cost_[i];++total;granted=true;break;
            }
            if(!granted)break;
        }
        // Force a rotating first opportunity for expensive lanes too: when a
        // lane cannot fit the whole quota, give it a frame on its turn.
        if(pending[first]&&caps[first]&&cost_[first]>quota_){out={};out[first]=1;}
        else if(!total)for(size_t j=0;j<Count;++j){size_t i=(first+j)%Count;if(pending[i]&&caps[i]){out[i]=1;break;}}
        cursor_=(first+1)%Count;return out;
    }
    double quotaMs() const noexcept{return quota_;}
    const Costs& estimatedCostMs() const noexcept{return cost_;}
private:
    double target_,quota_;Costs cost_{0.8,0.5,0.4,0.6,0.2};std::array<bool,Count> seen_{};
    uint64_t last_=0;size_t cursor_=0;unsigned fast_=0;
};
}
