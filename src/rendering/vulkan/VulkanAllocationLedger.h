#pragma once
#include "rendering/vulkan/DeviceMemoryUsage.h"
#include <vulkan/vulkan.h>
#include <map>
#include <mutex>

namespace OptiCraft::Vulkan {
// All explicit device memory allocations, including retired/in-flight buffers.
// Allocation handles are scoped to their logical device. No work on draw hot paths.
class VulkanAllocationLedger {
    struct Record {uint32_t heap;uint64_t bytes;};
    struct Device {std::map<VkDeviceMemory,Record> live;DeviceMemoryUsage usage;};
    std::mutex mutex_;
    std::map<VkDevice,Device> devices_;
public:
    static VulkanAllocationLedger& instance(){static VulkanAllocationLedger ledger;return ledger;}
    VkResult allocate(VkPhysicalDevice physical,VkDevice device,const VkMemoryAllocateInfo& info,VkDeviceMemory& memory){
        VkPhysicalDeviceMemoryProperties props{};vkGetPhysicalDeviceMemoryProperties(physical,&props);
        if(info.memoryTypeIndex>=props.memoryTypeCount||props.memoryTypes[info.memoryTypeIndex].heapIndex>=props.memoryHeapCount||
           props.memoryTypes[info.memoryTypeIndex].heapIndex>=16)return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        VkDeviceMemory allocated=VK_NULL_HANDLE;
        const auto result=vkAllocateMemory(device,&info,nullptr,&allocated);
        if(result!=VK_SUCCESS)return result;
        try {
            std::lock_guard lock(mutex_);
            auto& d=devices_[device];
            const auto heap=props.memoryTypes[info.memoryTypeIndex].heapIndex;
            d.live.emplace(allocated,Record{heap,info.allocationSize});
            d.usage.count=std::min<uint32_t>(props.memoryHeapCount,16);
            for(uint32_t i=0;i<d.usage.count;++i){
                d.usage.heaps[i].budget=props.memoryHeaps[i].size;
                d.usage.heaps[i].local=(props.memoryHeaps[i].flags&VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)!=0;
            }
            auto& h=d.usage.heaps[heap];h.tracked+=info.allocationSize;h.used=h.tracked;
            ++d.usage.allocations;
        } catch(...) {
            vkFreeMemory(device,allocated,nullptr);return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        memory=allocated;return VK_SUCCESS;
    }
    void free(VkDevice device,VkDeviceMemory memory) noexcept {
        if(!memory)return;
        std::lock_guard lock(mutex_);
        auto d=devices_.find(device);
        if(d!=devices_.end()){
            auto i=d->second.live.find(memory);
            if(i!=d->second.live.end()){
                auto& h=d->second.usage.heaps[i->second.heap];h.tracked-=i->second.bytes;h.used=h.tracked;
                --d->second.usage.allocations;d->second.live.erase(i);
            }
            if(d->second.live.empty())devices_.erase(d);
        }
        vkFreeMemory(device,memory,nullptr);
    }
    DeviceMemoryUsage snapshot(VkDevice device){
        std::lock_guard lock(mutex_);auto it=devices_.find(device);
        return it==devices_.end()?DeviceMemoryUsage{}:it->second.usage;
    }
};
inline VkResult allocateTrackedMemory(VkPhysicalDevice p,VkDevice d,const VkMemoryAllocateInfo& i,VkDeviceMemory& m){return VulkanAllocationLedger::instance().allocate(p,d,i,m);}
inline void freeTrackedMemory(VkDevice d,VkDeviceMemory m)noexcept{VulkanAllocationLedger::instance().free(d,m);}
inline DeviceMemoryUsage trackedMemory(VkDevice d){return VulkanAllocationLedger::instance().snapshot(d);}
}
