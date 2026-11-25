#pragma once
#include "rendering/vulkan/VulkanDeviceBuffer.h"
#include "rendering/vulkan/VulkanFrameRuntime.h"
#include <array>
#include <span>
namespace OptiCraft::Vulkan {
class VulkanAlphaSort {
public:
    ~VulkanAlphaSort(){shutdown();}
    VulkanAlphaSort()=default;
    VulkanAlphaSort(const VulkanAlphaSort&)=delete;
    VulkanAlphaSort& operator=(const VulkanAlphaSort&)=delete;
    void initialize(VulkanFrameRuntime&,const std::array<VkBuffer,VulkanFrameRuntime::kFramesInFlight>&,VkDeviceSize,VkBuffer,VkDeviceSize,VkBuffer,VkDeviceSize,std::span<const uint32_t>);
    void record(const VulkanFrameRuntime::Frame&,uint32_t,const std::array<float,4>&)const;
    VkDescriptorSetLayout layout()const{return vertexLayout_;}
    VkDescriptorSet set(uint32_t slot)const{return frames_.at(slot).vertexSet;}
    VkBuffer commands(uint32_t slot)const{return frames_.at(slot).commands.buffer();}
    VkDeviceSize depthCommandOffset(uint32_t slot)const{return frames_.at(slot).commands.size()/2;}
    void shutdown()noexcept;
private:
    struct Frame {VulkanDeviceBuffer commands,indices;VkDescriptorSet set=VK_NULL_HANDLE,vertexSet=VK_NULL_HANDLE;};
    std::array<Frame,VulkanFrameRuntime::kFramesInFlight> frames_{};
    VkDevice device_=VK_NULL_HANDLE;VkDescriptorSetLayout layout_=VK_NULL_HANDLE,vertexLayout_=VK_NULL_HANDLE;VkDescriptorPool pool_=VK_NULL_HANDLE;
    bool splitCutout_=true;
    VkPipelineLayout pipelineLayout_=VK_NULL_HANDLE;VkPipeline pipeline_=VK_NULL_HANDLE;
};
}
