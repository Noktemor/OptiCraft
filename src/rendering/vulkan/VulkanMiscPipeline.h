#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <span>

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
                    VkDescriptorSetLayout meshSetLayout,
                    std::span<const uint32_t> vertexSpv,
                    std::span<const uint32_t> fragmentSpv,
                    bool depthAttachment = true);
    // Call after compute writes are made visible to DRAW_INDIRECT / VERTEX_SHADER.
    // The draw commands use vertexCount=6; firstInstance indexes 32-byte quads.
    // Opaque/cutout is a depth-writing pass; alpha is drawn back-to-front with
    // depth testing and blending, but without writing depth.
    void recordDraws(VkCommandBuffer cmd, VkDescriptorSet meshSet,
                     VkDescriptorSet atlasSet, const float viewProjection[16],
                     VkBuffer commands, VkDeviceSize byteOffset, uint32_t count,
                     bool alpha, bool deviceMultiDrawIndirect,
                     uint32_t maxIndirectCount) const;
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
