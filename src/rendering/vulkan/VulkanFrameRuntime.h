#pragma once

#include <vulkan/vulkan.h>
#include "rendering/vulkan/DeviceMemoryUsage.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct GLFWwindow;

namespace OptiCraft::Vulkan {

// A real window/swapchain/frame-submission owner, independent of the OpenGL Game.
// The caller may record compute work before the render pass and graphics commands
// inside it. All framebuffer, sync and command-pool lifetimes are managed here.
// Only one thread calls beginFrame/endFrame; secondary command buffers may be
// recorded concurrently through VulkanParallelDraw's worker-owned pools.
class VulkanFrameRuntime {
public:
    static constexpr uint32_t kFramesInFlight = 2;
    enum class GpuPreference { Auto, Integrated, Discrete };

    struct Frame {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkRenderPass renderPass = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VkExtent2D extent{};
        uint32_t imageIndex = UINT32_MAX;
        uint32_t frameSlot = UINT32_MAX;
    };

    VulkanFrameRuntime() = default;
    ~VulkanFrameRuntime();
    VulkanFrameRuntime(const VulkanFrameRuntime&) = delete;
    VulkanFrameRuntime& operator=(const VulkanFrameRuntime&) = delete;

    void initialize(GLFWwindow* window, GpuPreference preference = GpuPreference::Auto,
                    bool enableValidation = false, bool vsync = true);
    // false when minimized or when the swapchain was recreated. No submission.
    bool beginFrame(Frame& frame);
    void beginColorPass(const Frame& frame, std::array<float, 4> clearColor,
                        bool recordSecondaryDraws = false, bool split = false);
    void resumeColorPass(const Frame& frame);
    bool supportsDepthPyramid() const noexcept { return firstPass_ && resumePass_; }
    VkImage depthImage(const Frame& frame) const { return depth_.at(frame.imageIndex).image; }
    VkImageView depthView(const Frame& frame) const { return depth_.at(frame.imageIndex).view; }
    void endColorPass(const Frame& frame);
    // Ends recording, submits once, presents once, then advances the frame slot.
    void endFrame(const Frame& frame);
    void shutdown() noexcept;

    VkInstance instance() const noexcept { return instance_; }
    VkPhysicalDevice physicalDevice() const noexcept { return physical_; }
    VkDevice device() const noexcept { return device_; }
    VkQueue graphicsQueue() const noexcept { return graphicsQueue_; }
    uint32_t graphicsQueueFamily() const noexcept { return graphicsFamily_; }
    VkRenderPass renderPass() const noexcept { return renderPass_; }
    VkExtent2D extent() const noexcept { return extent_; }
    VkFormat colorFormat() const noexcept { return format_; }
    VkFormat depthFormat() const noexcept { return depthFormat_; }
    bool supportsMultiDrawIndirect() const noexcept { return multiDrawIndirect_; }
    bool supportsShaderFloat64() const noexcept { return shaderFloat64_; }
    bool supportsIndirectCount() const noexcept { return indirectCount_; }
    const std::string& gpuName() const noexcept { return gpuName_; }
    DeviceMemoryUsage deviceMemoryUsage() const;
    uint64_t framesPresented() const noexcept { return framesPresented_; }
    uint64_t swapchainGeneration() const noexcept { return swapchainGeneration_; }

private:
    struct DepthImage {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };
    struct FrameSync {
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        VkFence completed = VK_NULL_HANDLE;
    };

    void selectDevice(GpuPreference preference);
    void refreshMemoryBudget();
    void createSwapchain();
    DepthImage createDepthImage() const;
    uint32_t deviceLocalMemoryType(uint32_t allowedBits) const;
    void destroySwapchain() noexcept;
    void recreateSwapchain();
    bool resizedOrMinimized() const;
    static void check(VkResult result, const char* operation);

    bool vsync_ = true;
    GLFWwindow* window_ = nullptr; // non-owning; alive until shutdown
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    uint32_t graphicsFamily_ = UINT32_MAX;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkFormat depthFormat_ = VK_FORMAT_UNDEFINED;
    VkPhysicalDeviceMemoryProperties memoryProperties_{};
    VkExtent2D extent_{};
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkRenderPass firstPass_ = VK_NULL_HANDLE, resumePass_ = VK_NULL_HANDLE;
    bool depthSampled_ = false, splitPending_ = false;
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<DepthImage> depth_;
    std::vector<VkFramebuffer> framebuffers_;
    // Per-image presentation semaphores: a frame fence does not prove that
    // vkQueuePresentKHR has finished waiting on that frame's semaphore.
    std::vector<VkSemaphore> presentReady_;
    std::vector<VkFence> imageInFlight_;
    std::array<FrameSync, kFramesInFlight> frames_{};
    uint32_t frameSlot_ = 0;
    uint32_t acquiredImage_ = UINT32_MAX;
    uint64_t framesPresented_ = 0;
    uint64_t swapchainGeneration_ = 0;
    bool frameOpen_ = false;
    bool colorPassOpen_ = false;
    bool colorPassRecorded_ = false;
    bool recreateRequested_ = false;
    bool multiDrawIndirect_ = false;
    bool indirectCount_ = false;
    bool shaderFloat64_ = false;
    bool memoryBudgetSupported_=false;
    uint32_t memoryBudgetTick_=0;
    DeviceMemoryUsage memoryBudgetSample_{};
    std::array<uint64_t,16> driverHeapBudget_{},driverHeapUsage_{};
    int windowFbWidth_ = 0, windowFbHeight_ = 0;
    std::string gpuName_;
};

} // namespace OptiCraft::Vulkan
