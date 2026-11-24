#include "rendering/vulkan/VulkanVoxelPipeline.h"
#include <array>
#include <stdexcept>
#include <string>

namespace OptiCraft::Vulkan {
namespace {
void checked(VkResult result, const char* action) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(action) + " VkResult=" + std::to_string(result));
}
VkShaderModule createShader(VkDevice device, std::span<const uint32_t> words) {
    if (words.empty() || words.front() != 0x07230203u)
        throw std::invalid_argument("Invalid SPIR-V for VulkanVoxelPipeline");
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = words.size_bytes(); info.pCode = words.data();
    VkShaderModule module = VK_NULL_HANDLE;
    checked(vkCreateShaderModule(device, &info, nullptr, &module),
            "vkCreateShaderModule(voxel)");
    return module;
}
}
VulkanVoxelPipeline::~VulkanVoxelPipeline() { shutdown(); }
void VulkanVoxelPipeline::initialize(VkDevice device, VkRenderPass renderPass,
                                     VkDescriptorSetLayout descriptor,
                                     std::span<const uint32_t> vertexSpv,
                                     std::span<const uint32_t> fragmentSpv,
                                     bool hasDepth) {
    if (device_ || !device || !renderPass || !descriptor)
        throw std::invalid_argument("VulkanVoxelPipeline::initialize bad handles");
    device_ = device;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    try {
        VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT,0,64};
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1; layoutInfo.pSetLayouts = &descriptor;
        layoutInfo.pushConstantRangeCount = 1; layoutInfo.pPushConstantRanges = &push;
        checked(vkCreatePipelineLayout(device_,&layoutInfo,nullptr,&layout_),
                "vkCreatePipelineLayout(voxel)");
        vs = createShader(device_,vertexSpv);
        fs = createShader(device_,fragmentSpv);
        std::array<VkPipelineShaderStageCreateInfo,2> stages{};
        for (auto& stage : stages) {
            stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stage.pName = "main";
        }
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs;
        VkPipelineVertexInputStateCreateInfo vertices{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        // Zero vertex bindings: gl_VertexIndex and gl_InstanceIndex pull GPU faces.
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = 1; viewport.scissorCount = 1;
        std::array<VkDynamicState,2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = uint32_t(dynamicStates.size());
        dynamic.pDynamicStates = dynamicStates.data();
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        // No culling in diagnostic pass; caller may use flipped Vulkan viewport.
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable = hasDepth ? VK_TRUE : VK_FALSE;
        depth.depthWriteEnable = hasDepth ? VK_TRUE : VK_FALSE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState attachment{};
        attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1; blend.pAttachments = &attachment;
        VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipelineInfo.stageCount = uint32_t(stages.size());
        pipelineInfo.pStages = stages.data();
        pipelineInfo.pVertexInputState = &vertices;
        pipelineInfo.pInputAssemblyState = &assembly;
        pipelineInfo.pViewportState = &viewport;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &samples;
        pipelineInfo.pDepthStencilState = &depth;
        pipelineInfo.pColorBlendState = &blend;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = layout_;
        pipelineInfo.renderPass = renderPass;
        pipelineInfo.subpass = 0;
        checked(vkCreateGraphicsPipelines(device_,VK_NULL_HANDLE,1,&pipelineInfo,nullptr,&pipeline_),
                "vkCreateGraphicsPipelines(voxel)");
    } catch (...) {
        if (vs) vkDestroyShaderModule(device_,vs,nullptr);
        if (fs) vkDestroyShaderModule(device_,fs,nullptr);
        shutdown();
        throw;
    }
    vkDestroyShaderModule(device_,vs,nullptr);
    vkDestroyShaderModule(device_,fs,nullptr);
}
void VulkanVoxelPipeline::shutdown() {
    if (!device_) return;
    if (pipeline_) vkDestroyPipeline(device_,pipeline_,nullptr);
    if (layout_) vkDestroyPipelineLayout(device_,layout_,nullptr);
    pipeline_ = VK_NULL_HANDLE; layout_ = VK_NULL_HANDLE; device_ = VK_NULL_HANDLE;
}
} // namespace OptiCraft::Vulkan
