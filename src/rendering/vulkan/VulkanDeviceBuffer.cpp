#include "rendering/vulkan/VulkanDeviceBuffer.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace OptiCraft::Vulkan {
namespace {
void check(VkResult r, const char* action) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(action)+" failed, VkResult="+std::to_string(r));
}
}
VulkanDeviceBuffer::VulkanDeviceBuffer(VulkanDeviceBuffer&& other) noexcept { *this=std::move(other); }
VulkanDeviceBuffer& VulkanDeviceBuffer::operator=(VulkanDeviceBuffer&& other) noexcept {
    if (this==&other) return *this;
    destroy();
    device_=std::exchange(other.device_,VK_NULL_HANDLE);
    buffer_=std::exchange(other.buffer_,VK_NULL_HANDLE);
    memory_=std::exchange(other.memory_,VK_NULL_HANDLE);
    size_=std::exchange(other.size_,0);
    allocationSize_=std::exchange(other.allocationSize_,0);
    atomSize_=std::exchange(other.atomSize_,1);
    memoryFlags_=std::exchange(other.memoryFlags_,0);
    mapping_=std::exchange(other.mapping_,nullptr);
    return *this;
}
void VulkanDeviceBuffer::create(VkPhysicalDevice physical,VkDevice device,VkDeviceSize bytes,
                                VkBufferUsageFlags usage,VkMemoryPropertyFlags required,
                                VkMemoryPropertyFlags preferred) {
    if (!physical || !device || !bytes || !usage || buffer_)
        throw std::invalid_argument("VulkanDeviceBuffer invalid create");
    device_=device;
    try {
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size=bytes;
        bufferInfo.usage=usage;
        bufferInfo.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device_,&bufferInfo,nullptr,&buffer_),"vkCreateBuffer");
        VkMemoryRequirements needs{};
        vkGetBufferMemoryRequirements(device_,buffer_,&needs);
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(physical,&memory);
        uint32_t chosen=UINT32_MAX;
        // Prefer device-local when possible, HOST_COHERENT for upload staging.
        for (uint32_t i=0;i<memory.memoryTypeCount;i++) {
            const auto flags=memory.memoryTypes[i].propertyFlags;
            if (!(needs.memoryTypeBits&(1u<<i)) || (flags&required)!=required) continue;
            if (chosen==UINT32_MAX || (flags&preferred)==preferred) {
                chosen=i;
                if ((flags&preferred)==preferred) break;
            }
        }
        if (chosen==UINT32_MAX) throw std::runtime_error("No suitable Vulkan buffer memory type");
        memoryFlags_=memory.memoryTypes[chosen].propertyFlags;
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize=needs.size;
        alloc.memoryTypeIndex=chosen;
        check(vkAllocateMemory(device_,&alloc,nullptr,&memory_),"vkAllocateMemory(buffer)");
        check(vkBindBufferMemory(device_,buffer_,memory_,0),"vkBindBufferMemory");
        if (memoryFlags_&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            check(vkMapMemory(device_,memory_,0,VK_WHOLE_SIZE,0,&mapping_),"vkMapMemory");
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical,&properties);
        atomSize_=std::max<VkDeviceSize>(1,properties.limits.nonCoherentAtomSize);
        size_=bytes;
        allocationSize_=needs.size;
    } catch (...) {destroy();throw;}
}
void VulkanDeviceBuffer::write(VkDeviceSize offset,const void* data,VkDeviceSize bytes) {
    if (!mapping_ || (!data && bytes) || offset>size_ || bytes>size_-offset)
        throw std::out_of_range("VulkanDeviceBuffer mapped write out of range");
    if (!bytes) return;
    std::memcpy(static_cast<std::byte*>(mapping_)+offset,data,size_t(bytes));
    if ((memoryFlags_&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0) return;
    const VkDeviceSize start=(offset/atomSize_)*atomSize_;
    const VkDeviceSize end=offset+bytes;
    const VkDeviceSize ceil=end>std::numeric_limits<VkDeviceSize>::max()-(atomSize_-1)
        ? allocationSize_ : ((end+atomSize_-1)/atomSize_)*atomSize_;
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory=memory_;
    range.offset=start;
    range.size=ceil>=allocationSize_ ? VK_WHOLE_SIZE : ceil-start;
    check(vkFlushMappedMemoryRanges(device_,1,&range),"vkFlushMappedMemoryRanges");
}
void VulkanDeviceBuffer::read(VkDeviceSize offset,void* destination,VkDeviceSize bytes) const {
    if (!mapping_ || (!destination && bytes) || offset>size_ || bytes>size_-offset)
        throw std::out_of_range("VulkanDeviceBuffer mapped read out of range");
    if(!bytes)return;
    if(!(memoryFlags_&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        const VkDeviceSize start=(offset/atomSize_)*atomSize_;
        const VkDeviceSize end=offset+bytes;
        const VkDeviceSize ceil=end>std::numeric_limits<VkDeviceSize>::max()-(atomSize_-1)
            ? allocationSize_ : ((end+atomSize_-1)/atomSize_)*atomSize_;
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory=memory_;range.offset=start;
        range.size=ceil>=allocationSize_?VK_WHOLE_SIZE:ceil-start;
        check(vkInvalidateMappedMemoryRanges(device_,1,&range),"vkInvalidateMappedMemoryRanges");
    }
    std::memcpy(destination,static_cast<const std::byte*>(mapping_)+offset,size_t(bytes));
}
void VulkanDeviceBuffer::destroy() noexcept {
    if (device_) {
        if (mapping_) vkUnmapMemory(device_,memory_);
        if (buffer_) vkDestroyBuffer(device_,buffer_,nullptr);
        if (memory_) vkFreeMemory(device_,memory_,nullptr);
    }
    device_=VK_NULL_HANDLE;buffer_=VK_NULL_HANDLE;memory_=VK_NULL_HANDLE;
    mapping_=nullptr;size_=allocationSize_=0;atomSize_=1;memoryFlags_=0;
}
} // namespace OptiCraft::Vulkan
