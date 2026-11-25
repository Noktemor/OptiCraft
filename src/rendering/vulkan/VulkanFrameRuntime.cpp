#include "rendering/vulkan/VulkanAllocationLedger.h"
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include "rendering/vulkan/VulkanFrameRuntime.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace OptiCraft::Vulkan {
namespace {
template <typename T, typename Enumerate>
std::vector<T> enumerate(Enumerate&& query, const char* what) {
    for (;;) {
        uint32_t capacity = 0;
        VkResult result = query(&capacity, nullptr);
        if (result != VK_SUCCESS)
            throw std::runtime_error(std::string(what) + ": count failed " + std::to_string(result));
        if (!capacity) return {};
        std::vector<T> entries(capacity);
        uint32_t written = capacity;
        result = query(&written, entries.data());
        if (result == VK_INCOMPLETE) continue; // list grew, re-query capacity
        if (result != VK_SUCCESS)
            throw std::runtime_error(std::string(what) + ": enumerate failed " + std::to_string(result));
        entries.resize(written);
        return entries;
    }
}

bool hasExtension(VkPhysicalDevice gpu, const char* required) {
    const auto available = enumerate<VkExtensionProperties>(
        [gpu](uint32_t* count, VkExtensionProperties* props) {
            return vkEnumerateDeviceExtensionProperties(gpu, nullptr, count, props);
        }, "vkEnumerateDeviceExtensionProperties");
    return std::any_of(available.begin(), available.end(), [required](const auto& p) {
        return std::strcmp(p.extensionName, required) == 0;
    });
}

VkSurfaceFormatKHR chooseFormat(const std::vector<VkSurfaceFormatKHR>& choices) {
    if (choices.empty()) throw std::runtime_error("GPU has no swapchain surface formats");
    if (choices.size() == 1 && choices[0].format == VK_FORMAT_UNDEFINED)
        return {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    for (const auto& f : choices)
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return f;
    for (const auto& f : choices)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return f;
    return choices[0];
}

VKAPI_ATTR VkBool32 VKAPI_CALL validationCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::cerr << "[Vulkan validation] " << (data && data->pMessage ? data->pMessage : "unknown") << '\n';
    return VK_FALSE;
}

VkCompositeAlphaFlagBitsKHR chooseAlpha(VkCompositeAlphaFlagsKHR supported) {
    for (VkCompositeAlphaFlagBitsKHR mode : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                              VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                              VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
                                              VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
        if (supported & mode) return mode;
    throw std::runtime_error("GPU has no supported swapchain composite alpha mode");
}
} // namespace

void VulkanFrameRuntime::check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed, VkResult=" + std::to_string(result));
}

VulkanFrameRuntime::~VulkanFrameRuntime() { shutdown(); }

void VulkanFrameRuntime::initialize(GLFWwindow* window, GpuPreference preference, bool enableValidation, bool vsync) {
    if (!window || instance_ != VK_NULL_HANDLE)
        throw std::invalid_argument("VulkanFrameRuntime requires a new GLFW_NO_API window");
    window_ = window;
    vsync_ = vsync;
    try {
        if (!glfwVulkanSupported()) throw std::runtime_error("GLFW cannot find a Vulkan loader/driver");
        uint32_t extensionCount = 0;
        const char** required = glfwGetRequiredInstanceExtensions(&extensionCount);
        if (!required || !extensionCount) throw std::runtime_error("GLFW Vulkan instance extensions unavailable");
        std::vector<const char*> extensions(required, required + extensionCount);
        const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
        if (enableValidation) {
            const auto instanceExtensions = enumerate<VkExtensionProperties>(
                [](uint32_t* n, VkExtensionProperties* p) {
                    return vkEnumerateInstanceExtensionProperties(nullptr, n, p);
                }, "vkEnumerateInstanceExtensionProperties");
            if (std::none_of(instanceExtensions.begin(), instanceExtensions.end(), [](const auto& ext) {
                    return std::strcmp(ext.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0;
                })) throw std::runtime_error("--validation requires VK_EXT_debug_utils");
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            const auto available = enumerate<VkLayerProperties>(
                [](uint32_t* n, VkLayerProperties* p) { return vkEnumerateInstanceLayerProperties(n, p); },
                "vkEnumerateInstanceLayerProperties");
            if (std::none_of(available.begin(), available.end(), [](const auto& layer) {
                    return std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0;
                })) throw std::runtime_error("--validation requested but VK_LAYER_KHRONOS_validation is absent");
        }
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "OptiCraft 0.13.1 - Vulkan test 2";
        app.applicationVersion = VK_MAKE_VERSION(0, 13, 1);
        app.pEngineName = "OptiCraft";
        app.engineVersion = VK_MAKE_VERSION(0, 13, 1);
        app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create.pApplicationInfo = &app;
        create.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        create.ppEnabledExtensionNames = extensions.data();
        create.enabledLayerCount = enableValidation ? 1u : 0u;
        create.ppEnabledLayerNames = enableValidation ? layers : nullptr;
        check(vkCreateInstance(&create, nullptr, &instance_), "vkCreateInstance");
        if (enableValidation) {
            auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
            if (!createMessenger) throw std::runtime_error("vkCreateDebugUtilsMessengerEXT unavailable");
            VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                   VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            info.pfnUserCallback = validationCallback;
            check(createMessenger(instance_, &info, nullptr, &debugMessenger_),
                  "vkCreateDebugUtilsMessengerEXT");
        }
        check(glfwCreateWindowSurface(instance_, window_, nullptr, &surface_), "glfwCreateWindowSurface");
        selectDevice(preference);

        for (auto& f : frames_) {
            VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool.queueFamilyIndex = graphicsFamily_;
            pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            check(vkCreateCommandPool(device_, &pool, nullptr, &f.commandPool), "vkCreateCommandPool");
            VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            alloc.commandPool = f.commandPool;
            alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            alloc.commandBufferCount = 1;
            check(vkAllocateCommandBuffers(device_, &alloc, &f.commandBuffer), "vkAllocateCommandBuffers");
            VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            check(vkCreateSemaphore(device_, &sem, nullptr, &f.acquired), "vkCreateSemaphore(acquire)");
            VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            check(vkCreateFence(device_, &fence, nullptr, &f.completed), "vkCreateFence");
        }
        createSwapchain();
    } catch (...) { shutdown(); throw; }
}

void VulkanFrameRuntime::selectDevice(GpuPreference preference) {
    auto devices = enumerate<VkPhysicalDevice>(
        [this](uint32_t* n, VkPhysicalDevice* p) { return vkEnumeratePhysicalDevices(instance_, n, p); },
        "vkEnumeratePhysicalDevices");
    VkPhysicalDevice best = VK_NULL_HANDLE;
    uint32_t bestFamily = UINT32_MAX;
    int bestScore = -1;
    VkPhysicalDeviceFeatures bestFeatures{};
    for (VkPhysicalDevice gpu : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(gpu, &properties);
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
        if (VK_VERSION_MAJOR(properties.apiVersion) < 1 ||
            (VK_VERSION_MAJOR(properties.apiVersion) == 1 && VK_VERSION_MINOR(properties.apiVersion) < 2))
            continue;
        if (preference == GpuPreference::Integrated &&
            properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) continue;
        if (preference == GpuPreference::Discrete &&
            properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) continue;
        VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceFeatures(gpu, &features);
        if (!features.drawIndirectFirstInstance || !hasExtension(gpu, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
        VkSurfaceCapabilitiesKHR caps{};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface_, &caps) != VK_SUCCESS ||
            !(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) continue;
        auto formats = enumerate<VkSurfaceFormatKHR>(
            [this, gpu](uint32_t* n, VkSurfaceFormatKHR* f) {
                return vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface_, n, f);
            }, "vkGetPhysicalDeviceSurfaceFormatsKHR");
        auto present = enumerate<VkPresentModeKHR>(
            [this, gpu](uint32_t* n, VkPresentModeKHR* m) {
                return vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface_, n, m);
            }, "vkGetPhysicalDeviceSurfacePresentModesKHR");
        if (formats.empty() || present.empty()) continue;
        uint32_t nFamilies = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &nFamilies, nullptr);
        std::vector<VkQueueFamilyProperties> families(nFamilies);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &nFamilies, families.data());
        for (uint32_t i = 0; i < nFamilies; ++i) {
            if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) ||
                !(families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) || !families[i].queueCount) continue;
            VkBool32 supportsPresent = VK_FALSE;
            if (vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, surface_, &supportsPresent) != VK_SUCCESS ||
                !supportsPresent) continue;
            const bool integrated = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
            const bool discrete = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
            int score = (discrete ? 300 : integrated ? 200 : 100);
            if (preference == GpuPreference::Integrated) score += integrated ? 1000 : 0;
            if (preference == GpuPreference::Discrete) score += discrete ? 1000 : 0;
            if (score > bestScore) {
                best = gpu;
                bestFamily = i;
                bestScore = score;
                bestFeatures = features;
            }
        }
    }
    if (!best) throw std::runtime_error(
        "No Vulkan 1.2 GPU with graphics+compute+present queue, swapchain and drawIndirectFirstInstance");
    physical_ = best;
    graphicsFamily_ = bestFamily;
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical_, &props);
    gpuName_ = props.deviceName;
    multiDrawIndirect_ = bestFeatures.multiDrawIndirect == VK_TRUE;
    vkGetPhysicalDeviceMemoryProperties(physical_, &memoryProperties_);
    for (const VkFormat format : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT,
                                  VK_FORMAT_D16_UNORM}) {
        VkFormatProperties available{};
        vkGetPhysicalDeviceFormatProperties(physical_, format, &available);
        if (available.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            depthFormat_ = format;
            VkImageFormatProperties sampledProps{};
            depthSampled_ = format != VK_FORMAT_D24_UNORM_S8_UINT &&
                (available.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) &&
                vkGetPhysicalDeviceImageFormatProperties(physical_,format,VK_IMAGE_TYPE_2D,
                    VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,
                    0,&sampledProps)==VK_SUCCESS;
            break;
        }
    }
    if (depthFormat_ == VK_FORMAT_UNDEFINED)
        throw std::runtime_error("Selected Vulkan GPU has no supported depth attachment format");
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = graphicsFamily_;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures enabled{};
    enabled.drawIndirectFirstInstance = VK_TRUE;
    enabled.multiDrawIndirect = bestFeatures.multiDrawIndirect;
    enabled.shaderFloat64 = bestFeatures.shaderFloat64;
    shaderFloat64_ = enabled.shaderFloat64 == VK_TRUE;
    VkPhysicalDeviceVulkan12Features supported12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    supported.pNext = &supported12;
    vkGetPhysicalDeviceFeatures2(physical_, &supported);
    VkPhysicalDeviceVulkan12Features enabled12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    indirectCount_ = multiDrawIndirect_ && supported12.drawIndirectCount;
    enabled12.drawIndirectCount = indirectCount_ ? VK_TRUE : VK_FALSE;
    memoryBudgetSupported_=hasExtension(physical_,VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME,VK_EXT_MEMORY_BUDGET_EXTENSION_NAME};
    VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    create.queueCreateInfoCount = 1;
    create.pQueueCreateInfos = &queue;
    create.pEnabledFeatures = &enabled;
    create.pNext = &enabled12;
    create.enabledExtensionCount = memoryBudgetSupported_?2u:1u;
    create.ppEnabledExtensionNames = extensions;
    check(vkCreateDevice(physical_, &create, nullptr, &device_), "vkCreateDevice");
    vkGetDeviceQueue(device_, graphicsFamily_, 0, &graphicsQueue_);
    if (!graphicsQueue_) throw std::runtime_error("No Vulkan graphics queue returned");
    refreshMemoryBudget();
}

void VulkanFrameRuntime::refreshMemoryBudget(){
    if(!memoryBudgetSupported_)return;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    props.pNext=&budget;
    // All allocations and admissions run on the owner thread. Sample before
    // the driver query so intervening growth is conservatively charged.
    memoryBudgetSample_=trackedMemory(device_);
    vkGetPhysicalDeviceMemoryProperties2(physical_,&props);
    for(uint32_t i=0;i<16;++i){driverHeapBudget_[i]=budget.heapBudget[i];driverHeapUsage_[i]=budget.heapUsage[i];}
}
DeviceMemoryUsage VulkanFrameRuntime::deviceMemoryUsage() const {
    auto usage=trackedMemory(device_);
    return memoryBudgetSupported_?withDriverBudget(usage,memoryBudgetSample_,driverHeapBudget_,driverHeapUsage_):usage;
}

uint32_t VulkanFrameRuntime::deviceLocalMemoryType(uint32_t allowedBits) const {
    for (uint32_t i = 0; i < memoryProperties_.memoryTypeCount; ++i)
        if ((allowedBits & (1u << i)) &&
            (memoryProperties_.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            return i;
    throw std::runtime_error("No Vulkan device-local memory type for depth image");
}

VulkanFrameRuntime::DepthImage VulkanFrameRuntime::createDepthImage() const {
    DepthImage image{};
    try {
        VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        create.imageType = VK_IMAGE_TYPE_2D;
        create.format = depthFormat_;
        create.extent = {extent_.width, extent_.height, 1};
        create.mipLevels = 1;
        create.arrayLayers = 1;
        create.samples = VK_SAMPLE_COUNT_1_BIT;
        create.tiling = VK_IMAGE_TILING_OPTIMAL;
        create.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if(depthSampled_) create.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
        create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        check(vkCreateImage(device_, &create, nullptr, &image.image), "vkCreateImage(depth)");
        VkMemoryRequirements required{};
        vkGetImageMemoryRequirements(device_, image.image, &required);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = required.size;
        alloc.memoryTypeIndex = deviceLocalMemoryType(required.memoryTypeBits);
        check(allocateTrackedMemory(physical_,device_,alloc,image.memory), "vkAllocateMemory(depth)");
        check(vkBindImageMemory(device_, image.image, image.memory, 0), "vkBindImageMemory(depth)");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = depthFormat_;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (depthFormat_ == VK_FORMAT_D24_UNORM_S8_UINT)
            view.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        check(vkCreateImageView(device_, &view, nullptr, &image.view), "vkCreateImageView(depth)");
    } catch (...) {
        if (image.view) vkDestroyImageView(device_, image.view, nullptr);
        if (image.image) vkDestroyImage(device_, image.image, nullptr);
        if (image.memory) freeTrackedMemory(device_,image.memory);
        throw;
    }
    return image;
}

void VulkanFrameRuntime::createSwapchain() {
    if (!device_ || !window_ || swapchain_) throw std::logic_error("Invalid swapchain creation state");
    VkSurfaceCapabilitiesKHR caps{};
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps),
          "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    const auto formats = enumerate<VkSurfaceFormatKHR>(
        [this](uint32_t* n, VkSurfaceFormatKHR* f) {
            return vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, n, f);
        }, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    const VkSurfaceFormatKHR selected = chooseFormat(formats);
    int width = 0, height = 0;
    glfwGetFramebufferSize(window_, &width, &height);
    if (width <= 0 || height <= 0) return; // minimized, retry when drawable
    windowFbWidth_ = width;
    windowFbHeight_ = height;
    if (caps.currentExtent.width != UINT32_MAX) extent_ = caps.currentExtent;
    else {
        extent_.width = std::clamp(static_cast<uint32_t>(width),
                                   caps.minImageExtent.width, caps.maxImageExtent.width);
        extent_.height = std::clamp(static_cast<uint32_t>(height),
                                    caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (!extent_.width || !extent_.height) return;
    const uint32_t imageCount = caps.maxImageCount
        ? std::min(std::max(caps.minImageCount + 1, kFramesInFlight), caps.maxImageCount)
        : std::max(caps.minImageCount + 1, kFramesInFlight);
    VkSwapchainCreateInfoKHR create{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    create.surface = surface_;
    create.minImageCount = imageCount;
    create.imageFormat = selected.format;
    create.imageColorSpace = selected.colorSpace;
    create.imageExtent = extent_;
    create.imageArrayLayers = 1;
    create.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create.preTransform = caps.currentTransform;
    create.compositeAlpha = chooseAlpha(caps.supportedCompositeAlpha);
    create.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync_) {
        const auto modes = enumerate<VkPresentModeKHR>([this](uint32_t* n,VkPresentModeKHR* m){
            return vkGetPhysicalDeviceSurfacePresentModesKHR(physical_,surface_,n,m);
        }, "vkGetPhysicalDeviceSurfacePresentModesKHR");
        for (auto preferred : {VK_PRESENT_MODE_IMMEDIATE_KHR,VK_PRESENT_MODE_MAILBOX_KHR}) {
            if (std::find(modes.begin(),modes.end(),preferred)!=modes.end()) {create.presentMode=preferred;break;}
        }
    }
    std::cout << "[Vulkan] Present mode: " << (create.presentMode==VK_PRESENT_MODE_IMMEDIATE_KHR?"IMMEDIATE":create.presentMode==VK_PRESENT_MODE_MAILBOX_KHR?"MAILBOX":"FIFO") << '\n';
    create.clipped = VK_TRUE;
    check(vkCreateSwapchainKHR(device_, &create, nullptr, &swapchain_), "vkCreateSwapchainKHR");
    format_ = selected.format;
    images_ = enumerate<VkImage>(
        [this](uint32_t* n, VkImage* images) { return vkGetSwapchainImagesKHR(device_, swapchain_, n, images); },
        "vkGetSwapchainImagesKHR");
    if (images_.empty()) throw std::runtime_error("vkGetSwapchainImagesKHR returned no images");

    VkAttachmentDescription color{};
    color.format = format_;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentDescription depth{};
    depth.format = depthFormat_;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &ref;
    subpass.pDepthStencilAttachment = &depthRef;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    const std::array<VkAttachmentDescription, 2> attachments = {color, depth};
    rp.attachmentCount = static_cast<uint32_t>(attachments.size());
    rp.pAttachments = attachments.data();
    rp.subpassCount = 1;
    rp.pSubpasses = &subpass;
    rp.dependencyCount = 1;
    rp.pDependencies = &dependency;
    check(vkCreateRenderPass(device_, &rp, nullptr, &renderPass_), "vkCreateRenderPass");
    // All three passes are compatible: only attachment load/store and layouts differ.
    if (depthSampled_) {
        auto splitAttachments = attachments;
        splitAttachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        splitAttachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        rp.pAttachments = splitAttachments.data();
        if (vkCreateRenderPass(device_, &rp, nullptr, &firstPass_) == VK_SUCCESS) {
            splitAttachments[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            splitAttachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            splitAttachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            splitAttachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            splitAttachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            if(vkCreateRenderPass(device_, &rp, nullptr, &resumePass_) != VK_SUCCESS) {
                vkDestroyRenderPass(device_, firstPass_, nullptr); firstPass_=VK_NULL_HANDLE; resumePass_=VK_NULL_HANDLE;
            }
        } else firstPass_=VK_NULL_HANDLE;
    }
    views_.reserve(images_.size());
    depth_.reserve(images_.size());
    framebuffers_.reserve(images_.size());
    presentReady_.reserve(images_.size());
    for (VkImage image : images_) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format_;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.baseMipLevel = 0;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.baseArrayLayer = 0;
        view.subresourceRange.layerCount = 1;
        VkImageView out = VK_NULL_HANDLE;
        check(vkCreateImageView(device_, &view, nullptr, &out), "vkCreateImageView");
        views_.push_back(out);
        depth_.push_back(createDepthImage());
        const std::array<VkImageView, 2> attachmentsForImage = {views_.back(), depth_.back().view};
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb.renderPass = renderPass_;
        fb.attachmentCount = static_cast<uint32_t>(attachmentsForImage.size());
        fb.pAttachments = attachmentsForImage.data();
        fb.width = extent_.width;
        fb.height = extent_.height;
        fb.layers = 1;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        check(vkCreateFramebuffer(device_, &fb, nullptr, &framebuffer), "vkCreateFramebuffer");
        framebuffers_.push_back(framebuffer);
        VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore ready = VK_NULL_HANDLE;
        check(vkCreateSemaphore(device_, &sem, nullptr, &ready), "vkCreateSemaphore(present)");
        presentReady_.push_back(ready);
    }
    imageInFlight_.assign(images_.size(), VK_NULL_HANDLE);
    recreateRequested_ = false;
    ++swapchainGeneration_;
}

bool VulkanFrameRuntime::resizedOrMinimized() const {
    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    if (w <= 0 || h <= 0) return true;
    return windowFbWidth_ != w || windowFbHeight_ != h;
}

void VulkanFrameRuntime::recreateSwapchain() {
    if (frameOpen_) throw std::logic_error("Cannot recreate swapchain with active frame");
    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    if (w == 0 || h == 0) return;
    // Resize only: the rare device-wide wait also retires presentation uses
    // of per-image semaphores. It is NEVER used in the ordinary render loop.
    check(vkDeviceWaitIdle(device_), "vkDeviceWaitIdle(resize)");
    destroySwapchain();
    createSwapchain();
}

bool VulkanFrameRuntime::beginFrame(Frame& frame) {
    if (!device_ || frameOpen_) throw std::logic_error("beginFrame invalid state");
    if(memoryBudgetTick_++%30u==0)refreshMemoryBudget();
    frame = {};
    if (resizedOrMinimized() || recreateRequested_ || !swapchain_) {
        recreateSwapchain();
        return false;
    }
    FrameSync& sync = frames_[frameSlot_];
    check(vkWaitForFences(device_, 1, &sync.completed, VK_TRUE, UINT64_MAX), "vkWaitForFences(frame)");
    uint32_t imageIndex = UINT32_MAX;
    const VkResult acquired = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                                      sync.acquired, VK_NULL_HANDLE, &imageIndex);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return false;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
        check(acquired, "vkAcquireNextImageKHR");
    if (imageIndex >= images_.size()) throw std::runtime_error("Swapchain returned invalid image index");
    if (acquired == VK_SUBOPTIMAL_KHR) recreateRequested_ = true;
    if (VkFence previous = imageInFlight_[imageIndex]; previous && previous != sync.completed)
        check(vkWaitForFences(device_, 1, &previous, VK_TRUE, UINT64_MAX), "vkWaitForFences(image)");
    // Always wait before resetting a per-frame pool. No per-draw allocations.
    check(vkResetCommandPool(device_, sync.commandPool, 0), "vkResetCommandPool(frame)");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(sync.commandBuffer, &begin), "vkBeginCommandBuffer(frame)");
    frame = {sync.commandBuffer, renderPass_, framebuffers_[imageIndex], extent_, imageIndex, frameSlot_};
    acquiredImage_ = imageIndex;
    frameOpen_ = true;
    colorPassOpen_ = false;
    colorPassRecorded_ = false;
    splitPending_ = false;
    return true;
}

void VulkanFrameRuntime::beginColorPass(const Frame& frame, std::array<float, 4> clearColor,
                                        bool recordSecondaryDraws, bool split) {
    if (!frameOpen_ || colorPassOpen_ || colorPassRecorded_ ||
        frame.frameSlot != frameSlot_ || frame.imageIndex != acquiredImage_ ||
        frame.cmd != frames_[frameSlot_].commandBuffer)
        throw std::logic_error("beginColorPass: frame or render-pass state invalid");
    std::array<VkClearValue, 2> clears{};
    std::copy(clearColor.begin(), clearColor.end(), clears[0].color.float32);
    clears[1].depthStencil.depth = 1.0f;
    clears[1].depthStencil.stencil = 0;
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    if(split && !supportsDepthPyramid()) throw std::logic_error("split pass unsupported");
    splitPending_ = split;
    begin.renderPass = split ? firstPass_ : frame.renderPass;
    begin.framebuffer = frame.framebuffer;
    begin.renderArea = {{0, 0}, frame.extent};
    begin.clearValueCount = static_cast<uint32_t>(clears.size());
    begin.pClearValues = clears.data();
    // Secondary draw buffers can be executed in a separate compatible pass.
    vkCmdBeginRenderPass(frame.cmd, &begin, recordSecondaryDraws
        ? VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS : VK_SUBPASS_CONTENTS_INLINE);
    colorPassOpen_ = true;
}

void VulkanFrameRuntime::resumeColorPass(const Frame& frame) {
    if(!frameOpen_ || colorPassOpen_ || !colorPassRecorded_ || !splitPending_ ||
       frame.frameSlot != frameSlot_ || frame.imageIndex != acquiredImage_ ||
       frame.cmd != frames_[frameSlot_].commandBuffer)
        throw std::logic_error("resumeColorPass invalid state");
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass=resumePass_; begin.framebuffer=frame.framebuffer;
    begin.renderArea={{0,0},frame.extent};
    vkCmdBeginRenderPass(frame.cmd,&begin,VK_SUBPASS_CONTENTS_INLINE);
    splitPending_=false; colorPassRecorded_=false; colorPassOpen_=true;
}

void VulkanFrameRuntime::endColorPass(const Frame& frame) {
    if (!frameOpen_ || !colorPassOpen_ || frame.frameSlot != frameSlot_ || frame.cmd != frames_[frameSlot_].commandBuffer)
        throw std::logic_error("endColorPass called outside the active frame");
    vkCmdEndRenderPass(frame.cmd);
    colorPassOpen_ = false;
    colorPassRecorded_ = true;
}

void VulkanFrameRuntime::endFrame(const Frame& frame) {
    if (!frameOpen_ || colorPassOpen_ || !colorPassRecorded_ || splitPending_ ||
        frame.frameSlot != frameSlot_ || frame.imageIndex != acquiredImage_ ||
        frame.cmd != frames_[frameSlot_].commandBuffer)
        throw std::logic_error("endFrame requires completed color/depth pass");
    FrameSync& sync = frames_[frameSlot_];
    check(vkEndCommandBuffer(frame.cmd), "vkEndCommandBuffer(frame)");
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkSemaphore done = presentReady_[frame.imageIndex];
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &sync.acquired;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frame.cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &done;
    check(vkResetFences(device_, 1, &sync.completed), "vkResetFences(frame)");
    check(vkQueueSubmit(graphicsQueue_, 1, &submit, sync.completed), "vkQueueSubmit(frame)");
    imageInFlight_[frame.imageIndex] = sync.completed;
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &done;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &frame.imageIndex;
    const VkResult result = vkQueuePresentKHR(graphicsQueue_, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        recreateRequested_ = true;
    else check(result, "vkQueuePresentKHR");
    if(result==VK_SUCCESS || result==VK_SUBOPTIMAL_KHR)++framesPresented_;
    frameOpen_ = false;
    colorPassRecorded_ = false;
    acquiredImage_ = UINT32_MAX;
    frameSlot_ = (frameSlot_ + 1) % kFramesInFlight;
}

void VulkanFrameRuntime::destroySwapchain() noexcept {
    if (!device_) return;
    for (auto semaphore : presentReady_) if (semaphore) vkDestroySemaphore(device_, semaphore, nullptr);
    presentReady_.clear();
    for (auto framebuffer : framebuffers_) if (framebuffer) vkDestroyFramebuffer(device_, framebuffer, nullptr);
    framebuffers_.clear();
    for (const auto& depth : depth_) {
        if (depth.view) vkDestroyImageView(device_, depth.view, nullptr);
        if (depth.image) vkDestroyImage(device_, depth.image, nullptr);
        if (depth.memory) freeTrackedMemory(device_,depth.memory);
    }
    depth_.clear();
    if(firstPass_) vkDestroyRenderPass(device_,firstPass_,nullptr);
    if(resumePass_) vkDestroyRenderPass(device_,resumePass_,nullptr);
    firstPass_=resumePass_=VK_NULL_HANDLE;
    if (renderPass_) vkDestroyRenderPass(device_, renderPass_, nullptr);
    renderPass_ = VK_NULL_HANDLE;
    for (auto view : views_) if (view) vkDestroyImageView(device_, view, nullptr);
    views_.clear();
    images_.clear();
    imageInFlight_.clear();
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
    format_ = VK_FORMAT_UNDEFINED;
    extent_ = {};
    windowFbWidth_ = windowFbHeight_ = 0;
}

void VulkanFrameRuntime::shutdown() noexcept {
    memoryBudgetSupported_=false;memoryBudgetTick_=0;memoryBudgetSample_={};driverHeapBudget_={};driverHeapUsage_={};
    if (device_) {
        // Shutdown/recovery only. Streaming/normal frame submissions never wait idle.
        vkDeviceWaitIdle(device_);
        destroySwapchain();
        for (auto& f : frames_) {
            if (f.completed) vkDestroyFence(device_, f.completed, nullptr);
            if (f.acquired) vkDestroySemaphore(device_, f.acquired, nullptr);
            if (f.commandPool) vkDestroyCommandPool(device_, f.commandPool, nullptr);
            f = {};
        }
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (surface_ && instance_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
    if (debugMessenger_ && instance_) {
        auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyMessenger) destroyMessenger(instance_, debugMessenger_, nullptr);
    }
    debugMessenger_ = VK_NULL_HANDLE;
    if (instance_) vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
    physical_ = VK_NULL_HANDLE;
    depthFormat_ = VK_FORMAT_UNDEFINED;
    depthSampled_=false; splitPending_=false;
    graphicsQueue_ = VK_NULL_HANDLE;
    graphicsFamily_ = UINT32_MAX;
    window_ = nullptr;
    frameOpen_ = false;
    colorPassOpen_ = false;
    colorPassRecorded_ = false;
    recreateRequested_ = false;
    frameSlot_ = 0;
    acquiredImage_ = UINT32_MAX;
}

} // namespace OptiCraft::Vulkan
