#pragma once
#include <algorithm>
#include <cstdint>
#include <cmath>

namespace OptiCraft::Vulkan {
// Adjust only from completed GPU Count/Emit timestamps; no CPU wait or FPS guess.
class MeshWorkBudget {
public:
    void reset(uint32_t initial,uint32_t maximum) noexcept {
        maximum_=std::max(4u,maximum);budget_=std::clamp(initial,4u,maximum_);lastMs_=0;fast_=0;
    }
    void sample(double ms,uint32_t work,bool allowGrowth=true) noexcept {
        if(!std::isfinite(ms)||ms<=0.0||!work)return;
        lastMs_=ms;
        // A single expensive chunk is indivisible, but never grow the batch on a slow sample.
        if(ms>3.5){
            const auto next=uint32_t(std::clamp(double(work)*2.5/ms,4.0,double(maximum_)));
            budget_=std::max(4u,std::min(budget_-1u,next));fast_=0;
        }else if(!allowGrowth){
            // Cheap cached Emit must not inflate the budget of the next Count.
            return;
        }else if(ms<1.5 && work>=budget_*3u/4u){
            if(++fast_>=3){budget_=std::min(maximum_,budget_+std::max(1u,budget_/4u));fast_=0;}
        }else fast_=0;
    }
    uint32_t budget() const noexcept {return budget_;}
    double lastMs() const noexcept {return lastMs_;}
private:
    uint32_t budget_=16,maximum_=32,fast_=0;
    double lastMs_=0;
};
} // namespace OptiCraft::Vulkan
