#include "rendering/vulkan/VulkanMiscPipeline.h"

#include <array>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <limits>

namespace OptiCraft::Vulkan {
namespace {
void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " VkResult=" + std::to_string(result));
}
VkShaderModule shader(VkDevice device, std::span<const uint32_t> spirv) {
    if (spirv.empty() || spirv[0] != 0x07230203u)
        throw std::invalid_argument("VulkanMiscPipeline: invalid SPIR-V");
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = spirv.size_bytes(); info.pCode = spirv.data();
    VkShaderModule handle = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device, &info, nullptr, &handle), "vkCreateShaderModule(misc)");
    return handle;
}
} // namespace
VulkanMiscPipeline::~VulkanMiscPipeline() { shutdown(); }
void VulkanMiscPipeline::initialize(VkDevice device, VkRenderPass renderPass,
                                    VkDescriptorSetLayout meshSetLayout,VkDescriptorSetLayout alphaLayout,
                                    std::span<const uint32_t> vertexSpv,
                                    std::span<const uint32_t> fragmentSpv,
                                    bool depthAttachment) {
    if (device_ || !device || !renderPass || !meshSetLayout)
        throw std::invalid_argument("VulkanMiscPipeline: invalid Vulkan handles");
    device_ = device;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    try {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo atlasInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        atlasInfo.bindingCount = 1; atlasInfo.pBindings = &binding;
        check(vkCreateDescriptorSetLayout(device_, &atlasInfo, nullptr, &atlasLayout_),
              "vkCreateDescriptorSetLayout(atlas)");
        std::array<VkDescriptorSetLayout, 3> sets{meshSetLayout, atlasLayout_,alphaLayout};
        VkPushConstantRange camera{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(WorldViewParams)};
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = uint32_t(sets.size()); layoutInfo.pSetLayouts = sets.data();
        layoutInfo.pushConstantRangeCount = 1; layoutInfo.pPushConstantRanges = &camera;
        check(vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &layout_),
              "vkCreatePipelineLayout(misc)");
        vs = shader(device_, vertexSpv); fs = shader(device_, fragmentSpv);
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        for (auto& s : stages) {s = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; s.pName = "main";}
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs;
        VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = 1; viewport.scissorCount = 1;
        std::array<VkDynamicState, 2> dynamics{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = uint32_t(dynamics.size()); dynamic.pDynamicStates = dynamics.data();
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        // Many shaped elements/foliage are very thin; their backfaces must be visible.
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blending.attachmentCount = 1; blending.pAttachments = &blendAttachment;
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable = depthAttachment ? VK_TRUE : VK_FALSE;
        depth.depthWriteEnable = depthAttachment ? VK_TRUE : VK_FALSE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount = uint32_t(stages.size());gp.pStages = stages.data();
        gp.pVertexInputState = &vertex;gp.pInputAssemblyState = &assembly;
        gp.pViewportState = &viewport;gp.pRasterizationState = &raster;
        gp.pMultisampleState = &multisample;gp.pDepthStencilState = &depth;
        gp.pColorBlendState = &blending;gp.pDynamicState = &dynamic;
        gp.layout = layout_;gp.renderPass = renderPass;gp.subpass = 0;
        const VkSpecializationMapEntry alphaEntry{0,0,sizeof(uint32_t)};
        uint32_t alphaPass=0;
        VkSpecializationInfo alphaSpec{1,&alphaEntry,sizeof(alphaPass),&alphaPass};
        stages[0].pSpecializationInfo=&alphaSpec;
        stages[1].pSpecializationInfo=&alphaSpec;
        check(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &cutout_),
              "vkCreateGraphicsPipelines(misc cutout)");
        blendAttachment.blendEnable = VK_TRUE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        depth.depthWriteEnable = VK_FALSE;
        alphaPass=1;
        check(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &alpha_),
              "vkCreateGraphicsPipelines(misc alpha)");
    } catch (...) {
        if (vs) vkDestroyShaderModule(device_, vs, nullptr);
        if (fs) vkDestroyShaderModule(device_, fs, nullptr);
        shutdown(); throw;
    }
    vkDestroyShaderModule(device_, vs, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
}
void VulkanMiscPipeline::recordDraws(VkCommandBuffer cmd, VkDescriptorSet meshSet,
                                     VkDescriptorSet atlasSet,VkDescriptorSet alphaSet, const WorldViewParams& view,
                                     VkBuffer commands, VkDeviceSize offset, uint32_t count,
                                     bool alpha, bool multiDraw,
                                     uint32_t maxIndirectCount) const {
    if (!count) return;
    if (!device_ || !cmd || !meshSet || !atlasSet || !commands ||
        offset % alignof(VkDrawIndirectCommand))
        throw std::invalid_argument("VulkanMiscPipeline: invalid indirect draw");
    if (count > (std::numeric_limits<VkDeviceSize>::max()-offset)/sizeof(VkDrawIndirectCommand))
        throw std::out_of_range("VulkanMiscPipeline: indirect offset overflow");
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, alpha ? alpha_ : cutout_);
    const VkDescriptorSet sets[3]{meshSet, atlasSet,alphaSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 3, sets, 0, nullptr);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(view), &view);
    if (multiDraw) {
        const uint32_t limit=std::max(1u,maxIndirectCount);
        for(uint32_t first=0;first<count;){
            uint32_t amount=std::min(count-first,limit);
            vkCmdDrawIndirect(cmd,commands,offset+VkDeviceSize(first)*sizeof(VkDrawIndirectCommand),
                              amount,sizeof(VkDrawIndirectCommand));
            first+=amount;
        }
    } else {
        for (uint32_t i = 0; i < count; ++i)
            vkCmdDrawIndirect(cmd, commands, offset + VkDeviceSize(i)*sizeof(VkDrawIndirectCommand),
                              1, sizeof(VkDrawIndirectCommand));
    }
}
void VulkanMiscPipeline::shutdown() {
    if (!device_) return;
    if (cutout_) vkDestroyPipeline(device_, cutout_, nullptr);
    if (alpha_) vkDestroyPipeline(device_, alpha_, nullptr);
    if (layout_) vkDestroyPipelineLayout(device_, layout_, nullptr);
    if (atlasLayout_) vkDestroyDescriptorSetLayout(device_, atlasLayout_, nullptr);
    cutout_ = VK_NULL_HANDLE; alpha_ = VK_NULL_HANDLE; layout_ = VK_NULL_HANDLE;
    atlasLayout_ = VK_NULL_HANDLE; device_ = VK_NULL_HANDLE;
}
} // namespace OptiCraft::Vulkan
