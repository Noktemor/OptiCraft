#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <span>

namespace OptiCraft::Vulkan {
// Diagnostic graphics pipeline for GPU-built PackedFace geometry.
// The renderer owns render pass, color/depth images and swapchain. No CPU meshes.
class VulkanVoxelPipeline {
public:
    VulkanVoxelPipeline() = default;
    ~VulkanVoxelPipeline();
    VulkanVoxelPipeline(const VulkanVoxelPipeline&) = delete;
    VulkanVoxelPipeline& operator=(const VulkanVoxelPipeline&) = delete;
    // Render pass must have 1 color attachment (location 0) and an optional
    // compatible depth attachment. Caller supplies matching hasDepth.
    void initialize(VkDevice device, VkRenderPass renderPass,
                    VkDescriptorSetLayout mesherDescriptorLayout,
                    std::span<const uint32_t> vertexSpv,
                    std::span<const uint32_t> fragmentSpv,
                    bool hasDepth = true);
    void shutdown(); // only after GPU has finished using this pipeline
    VkPipeline pipeline() const noexcept { return pipeline_; }
    VkPipelineLayout layout() const noexcept { return layout_; }
private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
};
} // namespace OptiCraft::Vulkan
