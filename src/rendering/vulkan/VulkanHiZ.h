#pragma once
#include "rendering/vulkan/VulkanDeviceBuffer.h"
#include "rendering/vulkan/VulkanFrameRuntime.h"
#include "rendering/vulkan/VulkanGpuCuller.h"
#include "rendering/vulkan/WorldViewParams.h"
#include <span>
namespace OptiCraft::Vulkan {
// Same-frame Hi-Z; one descriptor set and pyramid per fenced frame slot.
class VulkanHiZ {
public:
    ~VulkanHiZ(){shutdown();}
    VulkanHiZ()=default;
    VulkanHiZ(const VulkanHiZ&)=delete;
    VulkanHiZ& operator=(const VulkanHiZ&)=delete;
    void initialize(VulkanFrameRuntime& runtime,
        const std::array<VulkanCullerBuffers,VulkanFrameRuntime::kFramesInFlight>& buffers,
        std::span<const uint32_t> spv,
        const std::array<VkBuffer,VulkanFrameRuntime::kFramesInFlight>& miscCommands, VkBuffer miscQuads,VkDeviceSize miscQuadBytes);
    bool ready()const noexcept{return device_!=VK_NULL_HANDLE;}
    void record(VulkanFrameRuntime& runtime,const VulkanFrameRuntime::Frame& frame,
                const WorldViewParams& view,uint32_t count);
    void shutdown()noexcept;
private:
    struct alignas(16) Params {
        std::array<float,16> vp;
        std::array<float,4> eye;
        std::array<uint32_t,4> dims,op;
    };
    static_assert(sizeof(Params)==112);
    struct Slot {VulkanDeviceBuffer pyramid;VkDescriptorSet set=VK_NULL_HANDLE;VulkanCullerBuffers buffers;};
    std::array<Slot,VulkanFrameRuntime::kFramesInFlight> slots_{};
    VkDevice device_=VK_NULL_HANDLE;
    VkSampler sampler_=VK_NULL_HANDLE;
    VkDescriptorSetLayout layout_=VK_NULL_HANDLE;
    VkDescriptorPool pool_=VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_=VK_NULL_HANDLE;
    VkPipeline pipeline_=VK_NULL_HANDLE;
    uint32_t maxGroups_=0;
    VkExtent2D extent_{};
};
}
