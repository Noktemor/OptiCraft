#pragma once
#include "world/chunks/Chunk_Manager.h"
#include "world/generation/Gpu_Owned_Worldgen_Input.h"
#include <array>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace OptiCraft::Vulkan {
// Bounded CPU preparation for Stage64 policy requests. Owner thread only API;
// worker calls ONLY the immutable World_Generator, never Chunk_Manager/Vulkan.
// Manager and generator must outlive this queue; detach the policy callback,
// shutdown/join, then destroy or mutate the generator. No automatic registration.
// A taken Ready result transfers responsibility for its unbound claim to the
// caller (forward to start_prepared or cancel it). Shutdown covers untaken work.
class OwnedWorldgenPreparationQueue {
public:
    using Request=Chunk_Manager::Gpu_Generation_Request;
    static constexpr size_t capacity=8;
    struct Prepared {
        Request request;
        std::unique_ptr<Gpu_Owned_Worldgen_Input> input;
    };
    enum class Poll {Idle, Pending, Ready, Rejected, OutputBusy};
    struct Stats {size_t queued=0,running=0,ready=0,failed=0,draining=0;};

    OwnedWorldgenPreparationQueue(Chunk_Manager& manager,const World_Generator& generator)
        :manager_(manager),generator_(generator),dimension_(generator.get_dimension_id()),
         worker_([this]{run();}) {}
    OwnedWorldgenPreparationQueue(const OwnedWorldgenPreparationQueue&)=delete;
    OwnedWorldgenPreparationQueue& operator=(const OwnedWorldgenPreparationQueue&)=delete;
    ~OwnedWorldgenPreparationQueue(){shutdown();}

    // Safe inside the policy callback: no manager calls, expensive preparation,
    // allocation of input arrays or wait for a worker. Refusal leaves ownership
    // with the caller. Accepted work consumes a slot until taken/canceled/drained.
    bool try_enqueue(const Request& request) {
        if(!request.claim || request.dimension!=dimension_)return false;
        std::lock_guard lock(mutex_);
        if(stopping_ || nextOrder_==std::numeric_limits<uint64_t>::max())return false;
        Slot* free=nullptr;
        for(auto& slot:slots_) {
            if(slot.state==State::Empty){if(!free)free=&slot;continue;}
            if(slot.request.claim==request.claim || slot.request.position==request.position)return false;
        }
        if(!free)return false;
        free->request=request;
        free->order=++nextOrder_;
        free->state=State::Queued;
        cv_.notify_one();
        return true;
    }

    // Owner thread, outside policy callback. Running preparation cannot be
    // interrupted inside the generator; its slot is retained until it drains.
    bool cancel(uint64_t claim) {
        if(!claim)return false;
        Request request{};
        {
            std::lock_guard lock(mutex_);
            Slot* found=nullptr;
            for(auto& slot:slots_)if(slot.state!=State::Empty && slot.state!=State::Draining &&
                slot.request.claim==claim){found=&slot;break;}
            if(!found)return false;
            request=found->request;
            if(found->state==State::Running)found->state=State::Draining;
            else *found=Slot{};
        }
        manager_.cancel_gpu_chunk_claim(request.position,request.claim);
        return true;
    }

    // Reclaim obsolete queued/ready work promptly; never touch a new claim at
    // the same position. No manager access while the queue mutex is held.
    size_t prune_stale() {
        std::array<Request,capacity> requests{};
        size_t count=0;
        {
            std::lock_guard lock(mutex_);
            for(const auto& s:slots_)if(s.state!=State::Empty && s.state!=State::Draining)
                requests[count++]=s.request;
        }
        size_t canceled=0;
        for(size_t i=0;i<count;++i)if(!current(requests[i]) && cancel(requests[i].claim))++canceled;
        return canceled;
    }

    // Nonblocking completion retrieval. Call only when the downstream producer
    // can accept a result, or retain that result yourself. Failure/exception or
    // stale identity releases the pending claim. Output changes only on Ready.
    Poll poll(Prepared& output) {
        // Do not silently overwrite a previously transferred claim.
        if(output.input)return Poll::OutputBusy;
        Prepared next{};
        {
            std::lock_guard lock(mutex_);
            auto* slot=oldest(State::Ready,State::Failed);
            if(!slot) {
                for(const auto& s:slots_)if(s.state!=State::Empty)return Poll::Pending;
                return Poll::Idle;
            }
            next.request=slot->request;
            next.input=std::move(slot->input);
            *slot=Slot{};
        }
        const auto& r=next.request;
        if(!next.input || !current(r) ||
           !generator_.owned_gpu_identity_current(next.input->identity()) ||
           next.input->x()!=r.position.x || next.input->y()!=r.position.y || next.input->z()!=r.position.z) {
            manager_.cancel_gpu_chunk_claim(r.position,r.claim);
            return Poll::Rejected;
        }
        output=std::move(next);
        return Poll::Ready;
    }

    // Take an already completed column without waiting for other levels.
    // The oldest completed request selects the column; failed/stale levels
    // are canceled independently. Output must be empty; caller owns its claims.
    Poll poll_column(std::vector<Prepared>& output) {
        if(!output.empty())return Poll::OutputBusy;
        std::vector<Prepared> taken;
        taken.reserve(capacity); // allocate before releasing any queue slot
        output.reserve(capacity);
        {
            std::lock_guard lock(mutex_);
            auto* first=oldest(State::Ready,State::Failed);
            if(!first){
                for(const auto& s:slots_)if(s.state!=State::Empty)return Poll::Pending;
                return Poll::Idle;
            }
            const auto key=first->request;
            for(;;){
                Slot* next=nullptr;
                for(auto& s:slots_)if((s.state==State::Ready || s.state==State::Failed) &&
                    s.request.position.x==key.position.x && s.request.position.z==key.position.z &&
                    s.request.generation==key.generation && (!next || s.order<next->order))next=&s;
                if(!next)break;
                taken.push_back({next->request,std::move(next->input)});*next=Slot{};
            }
        }
        for(auto& entry:taken){
            const auto& r=entry.request;
            if(!entry.input || !current(r) || !generator_.owned_gpu_identity_current(entry.input->identity()) ||
               entry.input->x()!=r.position.x || entry.input->y()!=r.position.y || entry.input->z()!=r.position.z)
                manager_.cancel_gpu_chunk_claim(r.position,r.claim);
            else output.push_back(std::move(entry));
        }
        return output.empty()?Poll::Rejected:Poll::Ready;
    }

    Stats stats() const {
        std::lock_guard lock(mutex_);
        Stats result;
        for(const auto& s:slots_)switch(s.state) {
            case State::Queued:++result.queued;break;
            case State::Running:++result.running;break;
            case State::Ready:++result.ready;break;
            case State::Failed:++result.failed;break;
            case State::Draining:++result.draining;break;
            case State::Empty:break;
        }
        return result;
    }

    // Explicit lifecycle boundary: stop accepting, cancel all untaken claims,
    // and join the single worker before generator lifetime/configuration changes.
    // May wait for the currently executing preparation, never called per frame.
    void shutdown() {
        std::array<Request,capacity> requests{};
        size_t count=0;
        {
            std::lock_guard lock(mutex_);
            if(stopping_)return;
            stopping_=true;
            for(auto& s:slots_) {
                if(s.state==State::Empty || s.state==State::Draining)continue;
                requests[count++]=s.request;
                if(s.state==State::Running)s.state=State::Draining;
                else s=Slot{};
            }
        }
        cv_.notify_one();
        if(worker_.joinable())worker_.join();
        for(size_t i=0;i<count;++i)manager_.cancel_gpu_chunk_claim(requests[i].position,requests[i].claim);
    }

private:
    enum class State {Empty,Queued,Running,Ready,Failed,Draining};
    struct Slot {
        Request request{};
        uint64_t order=0;
        State state=State::Empty;
        std::unique_ptr<Gpu_Owned_Worldgen_Input> input;
    };
    bool current(const Request& r) const {
        return manager_.world_generator()==&generator_ && r.dimension==dimension_ &&
            manager_.gpu_chunk_claim_current(r.position,r.claim);
    }
    // mutex held; oldest accepted request wins among the requested states.
    // Column coalescing can finish later requests before unrelated queued work.
    Slot* oldest(State a,State b) {
        Slot* selected=nullptr;
        for(auto& s:slots_)if((s.state==a || s.state==b) && (!selected || s.order<selected->order))selected=&s;
        return selected;
    }
    void run() {
        for(;;) {
            std::array<Slot*,capacity> batch{};
            size_t count=0;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock,[this]{return stopping_ || oldest(State::Queued,State::Queued);});
                if(stopping_)return;
                auto* first=oldest(State::Queued,State::Queued);
                batch[count++]=first;first->state=State::Running;
                // Only already queued levels from the oldest column. No sleep
                // or artificial batching delay. Each claim remains independent.
                for(;;){
                    Slot* next=nullptr;
                    for(auto& s:slots_)if(s.state==State::Queued &&
                        s.request.position.x==first->request.position.x &&
                        s.request.position.z==first->request.position.z &&
                        s.request.generation==first->request.generation &&
                        (!next || s.order<next->order))next=&s;
                    if(!next)break;
                    batch[count++]=next;next->state=State::Running;
                }
            }
            std::array<std::unique_ptr<Gpu_Owned_Worldgen_Input>,capacity> results{};
            if(count>1)try {
                std::vector<int> ys;ys.reserve(count);
                for(size_t i=0;i<count;++i)ys.push_back(batch[i]->request.position.y);
                const auto& p=batch[0]->request.position;
                std::vector<Gpu_Owned_Worldgen_Input> levels;
                if(generator_.prepare_owned_gpu_column(p.x,p.z,ys,levels) && levels.size()==count){
                    for(size_t i=0;i<count;++i)if(levels[i].x()==p.x && levels[i].z()==p.z && levels[i].y()==ys[i])
                        results[i]=std::make_unique<Gpu_Owned_Worldgen_Input>(std::move(levels[i]));
                }
            }catch(...) {/* independently retry missing levels */}
            for(size_t i=0;i<count;++i)if(!results[i])try {
                {
                    std::lock_guard lock(mutex_);
                    if(stopping_ || batch[i]->state==State::Draining)continue;
                }
                const auto& p=batch[i]->request.position;
                auto input=std::make_unique<Gpu_Owned_Worldgen_Input>();
                if(generator_.prepare_owned_gpu_input(p.x,p.y,p.z,*input))results[i]=std::move(input);
            }catch(...) {/* individual fallback */}
            {
                std::lock_guard lock(mutex_);
                for(size_t i=0;i<count;++i){
                    auto& slot=*batch[i];
                    if(slot.state==State::Draining || stopping_)slot=Slot{};
                    else {
                        slot.input=std::move(results[i]);
                        slot.state=slot.input?State::Ready:State::Failed;
                    }
                }
            }
        }
    }

    Chunk_Manager& manager_;
    const World_Generator& generator_;
    const decltype(Request::dimension) dimension_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::array<Slot,capacity> slots_{};
    uint64_t nextOrder_=0;
    bool stopping_=false;
    std::thread worker_; // last: all shared state initialized before run()
};
} // namespace OptiCraft::Vulkan
