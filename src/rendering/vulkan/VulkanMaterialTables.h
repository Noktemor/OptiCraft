#pragma once
#include "rendering/vulkan/VulkanBlockCatalog.h"
#include "rendering/vulkan/VulkanDeviceBuffer.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace OptiCraft::Vulkan {

// Immutable per-world GPU catalog: every placed Block_ID is indexed by its
// actual 16-bit ID, not compacted/reordered. Built only after Block_Registry
// and the atlas metadata have loaded. No per-frame material uploads.
class VulkanMaterialTables {
public:
    VulkanMaterialTables() = default;
    ~VulkanMaterialTables() { shutdown(); }
    VulkanMaterialTables(const VulkanMaterialTables&) = delete;
    VulkanMaterialTables& operator=(const VulkanMaterialTables&) = delete;
    void initialize(VkPhysicalDevice physical, VkDevice device, const GpuBlockCatalog& catalog);
    // One-time bulk GPU transfer. The caller records into an open primary
    // command buffer BEFORE submitting the first mesh job; staging must outlive
    // that submission. This method rejects duplicate recordings.
    void recordUpload(VkCommandBuffer primary);
    void releaseStagingAfterCompletion(); // caller has waited for upload fence
    void shutdown() noexcept; // caller ensures GPU finished using tables
    VkBuffer opaqueFlags() const noexcept { return opaqueFlags_.buffer(); }
    VkBuffer blockDescriptors() const noexcept { return blocks_.buffer(); }
    VkBuffer shapeIndex() const noexcept { return indices_.buffer(); }
    VkBuffer shapeBoxes() const noexcept { return boxes_.buffer(); }
    VkBuffer faceUv() const noexcept { return faces_.buffer(); }
    VkDeviceSize blockDescriptorBytes() const noexcept { return blocks_.size(); }
    VkDeviceSize shapeIndexBytes() const noexcept { return indices_.size(); }
    VkDeviceSize shapeBoxBytes() const noexcept { return boxes_.size(); }
    VkDeviceSize faceUvBytes() const noexcept { return faces_.size(); }
private:
    VkDevice device_ = VK_NULL_HANDLE;
    VulkanDeviceBuffer opaqueFlags_, blocks_, indices_, boxes_, faces_, staging_;
    std::array<VkBufferCopy,5> copies_{};
    bool recorded_ = false;
};
} // namespace OptiCraft::Vulkan
