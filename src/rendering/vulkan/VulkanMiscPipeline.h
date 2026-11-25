#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <span>
#include "rendering/vulkan/WorldViewParams.h"

namespace OptiCraft::Vulkan {

// Material-aware vertex pulling for every non-opaque-cube block kind.
// GPU compute fills the geometry buffer; vkCmdDrawIndirect reads GPU commands.
// A separate descriptor set (set=1) supplies the actual atlas image/sampler.
// Does not own the atlas image, draw command buffer or compute descriptor set.
class VulkanMiscPipeline {
public:
    VulkanMiscPipeline() = default;
    ~VulkanMiscPipeline();
    VulkanMiscPipeline(const VulkanMiscPipeline&) = delete;
    VulkanMiscPipeline& operator=(const VulkanMiscPipeline&) = delete;

    void initialize(VkDevice device, VkRenderPass renderPass,
                    VkDescriptorSetLayout meshSetLayout,VkDescriptorSetLayout alphaLayout,
                    std::span<const uint32_t> vertexSpv,
                    std::span<const uint32_t> fragmentSpv,
                    bool depthAttachment = true);
    // Call after compute writes are made visible to DRAW_INDIRECT / VERTEX_SHADER.
    // vertexCount=6; firstInstance indexes quads or the frame permutation.
    // firstVertex=6 selects permutation for mixed depth-writing draws.
    // Opaque/cutout is a depth-writing pass; alpha is drawn in supplied order with
    // depth testing and blending, but without writing depth.
    void recordDraws(VkCommandBuffer cmd, VkDescriptorSet meshSet,
                     VkDescriptorSet atlasSet,VkDescriptorSet alphaSet, const WorldViewParams& view,
                     VkBuffer commands, VkDeviceSize byteOffset, uint32_t count,
                     bool alpha, bool deviceMultiDrawIndirect, uint32_t maxIndirectCount) const;
    void shutdown(); // device must be idle with respect to this pipeline
    VkDescriptorSetLayout atlasSetLayout() const noexcept { return atlasLayout_; }
private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout atlasLayout_ = VK_NULL_HANDLE;
    VkPipeline cutout_ = VK_NULL_HANDLE;
    VkPipeline alpha_ = VK_NULL_HANDLE;
};
} // namespace OptiCraft::Vulkan
