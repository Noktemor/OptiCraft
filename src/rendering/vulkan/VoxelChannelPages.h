#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <vector>

namespace OptiCraft::Vulkan {
inline constexpr uint32_t kVoxelAddressFlag=32u;
inline constexpr uint32_t kVoxelUniformBlock=64u;
inline constexpr uint32_t kVoxelUniformMeta=128u;
inline constexpr uint32_t kVoxelUniformLight=256u;
// Independent page ownership, indexed by stable slot; page reuse is ordered
// by the stream's same-queue transfer barrier. Growth preserves page indices.
class VoxelChannelPages {
public:
    struct Entry {uint32_t value=0; bool uniform=true; bool active=false;};
    void reset(uint32_t slots,uint32_t wordsPerPage,uint32_t initial=UINT32_MAX){
        stride_=wordsPerPage;entries_.assign(slots,{});free_.clear();free_.reserve(slots);
        capacity_=0;grow(initial==UINT32_MAX?slots:initial);
    }
    void grow(uint32_t capacity){
        if(capacity<capacity_ || capacity>entries_.size())throw std::logic_error("Invalid voxel page growth");
        for(uint32_t i=capacity;i>capacity_;--i)free_.push_back(i-1);
        capacity_=capacity;
    }
    // Only trim an unused suffix; live offsets never move. A fragmented
    // arena may have to retain high pages even when its occupancy is low.
    uint32_t usedExtent() const noexcept {
        uint32_t extent=0;
        for(const auto& e:entries_)if(e.active && !e.uniform)
            extent=std::max(extent,e.value/stride_+1u);
        return extent;
    }
    void shrink(uint32_t capacity){
        if(!capacity || capacity>capacity_ || capacity<usedExtent())
            throw std::logic_error("Invalid voxel page shrink");
        std::erase_if(free_,[&](uint32_t page){return page>=capacity;});
        // Refill lower holes before extending the used suffix again.
        std::sort(free_.begin(),free_.end(),std::greater<uint32_t>{});
        capacity_=capacity;
    }
    uint32_t capacity() const noexcept{return capacity_;}
    void release(uint32_t slot){
        auto& e=entries_.at(slot);
        if(e.active && !e.uniform)free_.push_back(e.value/stride_);
        e={};
    }
    // Admission must reserve capacity before committing a dense packet.
    Entry set(uint32_t slot,bool uniform,uint32_t word){
        auto& e=entries_.at(slot);
        if(uniform){release(slot);e={word,true,true};}
        else if(!e.active || e.uniform){
            if(free_.empty())throw std::logic_error("Voxel channel page invariant violated");
            e={free_.back()*stride_,false,true};free_.pop_back();
        }
        return e;
    }
    const Entry& at(uint32_t slot) const{return entries_.at(slot);}
    uint32_t densePages() const{return uint32_t(capacity_-free_.size());}
private:
    uint32_t stride_=0,capacity_=0;
    std::vector<Entry> entries_;
    std::vector<uint32_t> free_;
};
}
