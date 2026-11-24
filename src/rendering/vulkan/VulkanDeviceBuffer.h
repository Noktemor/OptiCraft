#pragma once

#include <vulkan/vulkan.h>
#include <cstddef>
#include <cstdint>

namespace OptiCraft::Vulkan {

// VkDeviceMemory-owned persistent buffer. Creation is rare (startup/rebuild),
// never performed for an individual chunk or draw. The caller owns GPU lifetime:
// release this only after all referencing submissions have completed.
class VulkanDeviceBuffer {
public:
    VulkanDeviceBuffer() = default;
    ~VulkanDeviceBuffer() { destroy(); }
    VulkanDeviceBuffer(const VulkanDeviceBuffer&) = delete;
    VulkanDeviceBuffer& operator=(const VulkanDeviceBuffer&) = delete;
    VulkanDeviceBuffer(VulkanDeviceBuffer&& other) noexcept;
    VulkanDeviceBuffer& operator=(VulkanDeviceBuffer&& other) noexcept;

    void create(VkPhysicalDevice physical, VkDevice device, VkDeviceSize bytes,
                VkBufferUsageFlags usage, VkMemoryPropertyFlags required,
                VkMemoryPropertyFlags preferred = 0);
    void destroy() noexcept;
    // For HOST_VISIBLE buffers; flushes only affected noncoherent atoms.
    void write(VkDeviceSize offset, const void* bytes, VkDeviceSize size);
    // Readback only AFTER the GPU submission fence has signalled. Invalidates
    // the affected noncoherent atom range before accessing mapped memory.
    void read(VkDeviceSize offset, void* destination, VkDeviceSize bytes) const;
    [[nodiscard]] VkBuffer buffer() const noexcept { return buffer_; }
    [[nodiscard]] VkDeviceSize size() const noexcept { return size_; }
    [[nodiscard]] explicit operator bool() const noexcept { return buffer_ != VK_NULL_HANDLE; }
private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkDeviceSize size_ = 0;
    VkDeviceSize allocationSize_ = 0;
    VkDeviceSize atomSize_ = 1;
    VkMemoryPropertyFlags memoryFlags_ = 0;
    void* mapping_ = nullptr;
};

} // namespace OptiCraft::Vulkan
