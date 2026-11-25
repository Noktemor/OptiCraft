#pragma once
#include "rendering/vulkan/FrameVoxelDescriptors.h"
#include "rendering/vulkan/VulkanChunkStreamGpu.h"
#include "rendering/vulkan/VulkanChunkMeshing.h"
#include "rendering/vulkan/VulkanMaterialTables.h"
#include <array>
#include <span>

namespace OptiCraft::Vulkan {

// Real tile-atlas opaque block rendering from GPU-generated packed 8B faces.
// Descriptor set 0 is the vertex-pulling material table; set 1 is the same
// atlas descriptor-set layout used by the misc block pipeline.
class VulkanOpaqueMaterialPipeline {
public:
    ~VulkanOpaqueMaterialPipeline(){shutdown();}
    void initialize(VkDevice device,VkRenderPass renderPass,VkDescriptorSetLayout atlasLayout,
                    const VulkanChunkStreamGpu& stream,const VulkanChunkMeshing& mesh,
                    const VulkanMaterialTables& materials,std::span<const uint32_t> vertex,
                    std::span<const uint32_t> fragment);
    void shutdown()noexcept;
    VkPipeline pipeline()const noexcept{return pipeline_;}
    VkPipelineLayout layout()const noexcept{return layout_;}
    VkDescriptorSet descriptorSet()const noexcept{return descriptor_;}
    void selectVoxelFrame(uint32_t frame,uint64_t epoch,const std::array<VkBuffer,3>& buffers,
                          const std::array<VkDeviceSize,3>& sizes) {
        descriptor_=frameDescriptors_.select(device_,frame,epoch,{6,4,5},buffers,sizes);
    }
private:
    VkDevice device_=VK_NULL_HANDLE;
    VkPipeline pipeline_=VK_NULL_HANDLE;
    VkPipelineLayout layout_=VK_NULL_HANDLE;
    VkDescriptorSetLayout meshLayout_=VK_NULL_HANDLE;
    VkDescriptorPool pool_=VK_NULL_HANDLE;
    VkDescriptorSet descriptor_=VK_NULL_HANDLE;
    FrameVoxelDescriptors frameDescriptors_;
};
} // namespace OptiCraft::Vulkan
