#include "rendering/vulkan/VulkanParallelDraw.h"
#include "utils/concurrency/Render_Worker_Pool.h"

#include <algorithm>
#include <vector>
#include <atomic>
#include <stdexcept>
#include <string>

namespace OptiCraft::Vulkan {
namespace {
void checked(VkResult result, const char* what) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(what) + " VkResult=" + std::to_string(result));
}
}
VulkanParallelDraw::VulkanParallelDraw() = default;
VulkanParallelDraw::~VulkanParallelDraw() { shutdown(); }

void VulkanParallelDraw::initialize(VkDevice device, uint32_t queueFamily,
                                    uint32_t frames, uint32_t maxIndirect,
                                    bool supportsMultiDraw) {
    if (device_ || !device || !frames) throw std::invalid_argument("Invalid VulkanParallelDraw initialize");
    device_ = device;
    frameCount_ = frames;
    maxIndirect_ = std::max(1u, maxIndirect);
    multiDraw_ = supportsMultiDraw;
    workers_ = std::make_unique<Render_Worker_Pool>();
    workersPerFrame_ = 1 + std::min<uint32_t>(7u, static_cast<uint32_t>(workers_->worker_count()));
    try {
        slots_.resize(size_t(frameCount_) * workersPerFrame_);
        for (auto& slot : slots_) {
            VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            ci.queueFamilyIndex = queueFamily;
            ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            checked(vkCreateCommandPool(device_, &ci, nullptr, &slot.pool), "vkCreateCommandPool");
            VkCommandBufferAllocateInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            bi.commandPool = slot.pool;
            bi.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
            bi.commandBufferCount = 1;
            checked(vkAllocateCommandBuffers(device_, &bi, &slot.command), "vkAllocateCommandBuffers");
        }
    } catch (...) { shutdown(); throw; }
}

void VulkanParallelDraw::record(VkCommandBuffer primary, uint32_t frameSlot,
                                 VkRenderPass renderPass, VkFramebuffer framebuffer,
                                 VkPipeline pipeline, VkPipelineLayout layout,
                                 VkDescriptorSet descriptor, const float* mvp,
                                 VkBuffer indirect, uint32_t draws, VkExtent2D viewportExtent,
                                 VkDescriptorSet atlasSet) {
    if (!draws) return; // no needless Vulkan work
    if (!device_ || frameSlot >= frameCount_ || !primary || !renderPass || !framebuffer ||
        !pipeline || !layout || !descriptor || !mvp || !indirect ||
        !viewportExtent.width || !viewportExtent.height)
        throw std::invalid_argument("VulkanParallelDraw::record invalid argument");

    // Do not spin up command workers for a trivially small draw array.
    const uint32_t n = draws < 256 ? 1 : std::min(workersPerFrame_, (draws + 255u) / 256u);
    std::vector<VkCommandBuffer> buffers(n, VK_NULL_HANDLE);

    std::atomic<int> failure{VK_SUCCESS};
    workers_->parallel_for(n, 2, [&](size_t index) {
        Worker& worker = slots_[size_t(frameSlot) * workersPerFrame_ + index];
        // Distinct VkCommandPool per worker and frame. Frame fence/timeline is
        // guaranteed complete before reset by the caller.
        if (VkResult result = vkResetCommandPool(device_, worker.pool, 0); result != VK_SUCCESS) {
            failure.store(result, std::memory_order_relaxed);
            return;
        }
        VkCommandBufferInheritanceInfo inheritance{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
        inheritance.renderPass = renderPass;
        inheritance.subpass = 0;
        inheritance.framebuffer = framebuffer;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT |
                      VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
        begin.pInheritanceInfo = &inheritance;
        if (VkResult result = vkBeginCommandBuffer(worker.command, &begin); result != VK_SUCCESS) {
            failure.store(result, std::memory_order_relaxed);
            return;
        }

        vkCmdBindPipeline(worker.command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        const VkViewport viewport{0.0f,0.0f,float(viewportExtent.width),
                                  float(viewportExtent.height),0.0f,1.0f};
        const VkRect2D scissor{{0,0},viewportExtent};
        vkCmdSetViewport(worker.command,0,1,&viewport);
        vkCmdSetScissor(worker.command,0,1,&scissor);
        if(atlasSet){
            const VkDescriptorSet sets[2]{descriptor,atlasSet};
            vkCmdBindDescriptorSets(worker.command,VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    layout,0,2,sets,0,nullptr);
        }else{
            vkCmdBindDescriptorSets(worker.command,VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    layout,0,1,&descriptor,0,nullptr);
        }
        vkCmdPushConstants(worker.command, layout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, 16 * sizeof(float), mvp);

        const uint32_t start = static_cast<uint32_t>((uint64_t(draws) * index) / n);
        const uint32_t end = static_cast<uint32_t>((uint64_t(draws) * (index + 1)) / n);
        if (multiDraw_) {
            for (uint32_t first = start; first < end; ) {
                uint32_t count = std::min(maxIndirect_, end - first);
                vkCmdDrawIndirect(worker.command, indirect,
                                  VkDeviceSize(first) * sizeof(VkDrawIndirectCommand),
                                  count, sizeof(VkDrawIndirectCommand));
                first += count;
            }
        } else {
            for (uint32_t i = start; i < end; ++i)
                vkCmdDrawIndirect(worker.command, indirect,
                                  VkDeviceSize(i) * sizeof(VkDrawIndirectCommand),
                                  1, sizeof(VkDrawIndirectCommand));
        }
        if (VkResult result = vkEndCommandBuffer(worker.command); result != VK_SUCCESS) {
            failure.store(result, std::memory_order_relaxed);
            return;
        }
        buffers[index] = worker.command;
    });
    checked(static_cast<VkResult>(failure.load(std::memory_order_relaxed)),
            "Vulkan parallel secondary command recording");
    vkCmdExecuteCommands(primary, n, buffers.data());
}

void VulkanParallelDraw::shutdown() {
    if (!device_) return;
    // Join all worker tasks before destroying command pools.
    workers_.reset();
    for (auto& worker : slots_) {
        if (worker.pool) vkDestroyCommandPool(device_, worker.pool, nullptr);
    }
    slots_.clear();
    device_ = VK_NULL_HANDLE;
    frameCount_ = workersPerFrame_ = 0;
}
} // namespace OptiCraft::Vulkan
