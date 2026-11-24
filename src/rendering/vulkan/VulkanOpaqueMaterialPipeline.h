#pragma once
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
private:
    VkDevice device_=VK_NULL_HANDLE;
    VkPipeline pipeline_=VK_NULL_HANDLE;
    VkPipelineLayout layout_=VK_NULL_HANDLE;
    VkDescriptorSetLayout meshLayout_=VK_NULL_HANDLE;
    VkDescriptorPool pool_=VK_NULL_HANDLE;
    VkDescriptorSet descriptor_=VK_NULL_HANDLE;
};
} // namespace OptiCraft::Vulkan
