#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

class Render_Worker_Pool;

namespace OptiCraft::Vulkan {

// CPU records commands; geometry and visibility are produced exclusively on GPU.
// Each independently recorded secondary command buffer owns its own VkCommandPool.
// vkCmdDrawIndirect batches thousands of per-chunk commands into a few API calls.
class VulkanParallelDraw {
public:
    VulkanParallelDraw();
    ~VulkanParallelDraw();
    VulkanParallelDraw(const VulkanParallelDraw&) = delete;
    VulkanParallelDraw& operator=(const VulkanParallelDraw&) = delete;

    void initialize(VkDevice device, uint32_t graphicsQueueFamily,
                    uint32_t frameCount, uint32_t maxDrawIndirectCount,
                    bool multiDrawIndirectSupported);
    // All prior GPU submissions for frameSlot MUST be complete before reuse.
    // Primary command buffer must already be inside vkCmdBeginRenderPass with
    // VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS and matching renderPass/framebuffer.
    void record(VkCommandBuffer primary, uint32_t frameSlot,
                VkRenderPass renderPass, VkFramebuffer framebuffer,
                VkPipeline graphicsPipeline, VkPipelineLayout graphicsLayout,
                VkDescriptorSet descriptorSet, const float* viewProjection16,
                VkBuffer indirectCommands, uint32_t drawCount,
                VkExtent2D viewportExtent,
                VkDescriptorSet atlasSet = VK_NULL_HANDLE);
    void shutdown(); // caller waits for in-flight GPU execution before shutdown
private:
    struct Worker {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer command = VK_NULL_HANDLE;
    };
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t frameCount_ = 0;
    uint32_t workersPerFrame_ = 0;
    uint32_t maxIndirect_ = 1;
    bool multiDraw_ = false;
    std::unique_ptr<Render_Worker_Pool> workers_;
    std::vector<Worker> slots_;
};

} // namespace OptiCraft::Vulkan
