#pragma once
#include <array>
#include <cstddef>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace OptiCraft::Vulkan {
// Single writer, fixed slots. No disk work on the render thread. Results must
// be consumed before reuse. Destruction drains accepted jobs, not GPU claims.
template<class Job, size_t Slots=2> class BoundedPersistenceWorker {
public:
    enum class State {Empty,Queued,Running,Success,Failed};
    using Writer=bool(*)(const Job&);
    explicit BoundedPersistenceWorker(Writer writer):writer_(writer),thread_([this]{run();}){}
    ~BoundedPersistenceWorker(){ {std::lock_guard lock(mutex_); stop_=true;} cv_.notify_one(); thread_.join(); }
    bool submit(size_t slot,std::shared_ptr<const Job> job){
        std::lock_guard lock(mutex_);
        if(stop_||slot>=Slots||!job||slots_[slot].state!=State::Empty)return false;
        slots_[slot]={State::Queued,std::move(job)};cv_.notify_one();return true;
    }
    State poll(size_t slot){
        std::lock_guard lock(mutex_);if(slot>=Slots)return State::Empty;
        auto& s=slots_[slot];const auto state=s.state;
        if(state==State::Success||state==State::Failed)s={};
        return state;
    }
private:
    struct Slot {State state=State::Empty;std::shared_ptr<const Job> job;};
    void run(){
        std::unique_lock lock(mutex_);
        for(;;){
            cv_.wait(lock,[&]{if(stop_)return true;for(auto& s:slots_)if(s.state==State::Queued)return true;return false;});
            size_t i=0;while(i<Slots&&slots_[i].state!=State::Queued)++i;
            if(i==Slots){if(stop_)return;continue;}
            auto job=slots_[i].job;slots_[i].state=State::Running;lock.unlock();
            bool ok=false;try{ok=writer_&&writer_(*job);}catch(...){}
            lock.lock();slots_[i].state=ok?State::Success:State::Failed;
        }
    }
    Writer writer_;std::mutex mutex_;std::condition_variable cv_;std::array<Slot,Slots> slots_{};
    bool stop_=false;std::thread thread_;
};
}
