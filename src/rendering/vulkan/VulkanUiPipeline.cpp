#include "rendering/vulkan/VulkanUiPipeline.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>
namespace OptiCraft::Vulkan {
namespace {
void check(VkResult r,const char* where){if(r!=VK_SUCCESS)throw std::runtime_error(std::string(where)+": "+std::to_string(r));}
VkShaderModule makeShader(VkDevice device,std::span<const uint32_t> code){
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize=code.size_bytes();ci.pCode=code.data();VkShaderModule shader=VK_NULL_HANDLE;
    check(vkCreateShaderModule(device,&ci,nullptr,&shader),"UI shader");return shader;
}
}
void VulkanUiPipeline::initialize(VulkanFrameRuntime& rt,std::span<const uint32_t> vs,std::span<const uint32_t> fs){
    device_=rt.device();physical_=rt.physicalDevice();
    try {
        atlas_.initialize(physical_,device_,"assets/texture/ui_atlas.png",1);
        VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};dl.bindingCount=1;dl.pBindings=&binding;
        check(vkCreateDescriptorSetLayout(device_,&dl,nullptr,&setLayout_),"UI descriptor layout");
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&setLayout_;
        check(vkCreatePipelineLayout(device_,&pl,nullptr,&layout_),"UI pipeline layout");
        recreate(rt.renderPass(),vs,fs);
    }catch(...){shutdown();throw;}
}
void VulkanUiPipeline::recreate(VkRenderPass pass,std::span<const uint32_t> vsCode,std::span<const uint32_t> fsCode){
    if(pipeline_)vkDestroyPipeline(device_,pipeline_,nullptr);pipeline_=VK_NULL_HANDLE;
    VkShaderModule vs=VK_NULL_HANDLE,fs=VK_NULL_HANDLE;
    try {
        vs=makeShader(device_,vsCode);fs=makeShader(device_,fsCode);
        std::array<VkPipelineShaderStageCreateInfo,2> stages{};
        for(auto& s:stages){s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;s.pName="main";}
        stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vs;
        stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fs;
        const VkVertexInputBindingDescription binding{0,sizeof(UiVertex),VK_VERTEX_INPUT_RATE_VERTEX};
        const std::array<VkVertexInputAttributeDescription,3> attributes{{
            {0,0,VK_FORMAT_R32G32_SFLOAT,offsetof(UiVertex,x)},
            {1,0,VK_FORMAT_R32G32_SFLOAT,offsetof(UiVertex,u)},
            {2,0,VK_FORMAT_R32G32B32A32_SFLOAT,offsetof(UiVertex,r)}}};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount=1;vi.pVertexBindingDescriptions=&binding;vi.vertexAttributeDescriptionCount=3;vi.pVertexAttributeDescriptions=attributes.data();
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=1;vp.scissorCount=1;
        const std::array<VkDynamicState,2> states{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dy.dynamicStateCount=2;dy.pDynamicStates=states.data();
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;rs.lineWidth=1.f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        VkPipelineColorBlendAttachmentState attachment{};
        attachment.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
        attachment.blendEnable=VK_TRUE;attachment.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;attachment.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;attachment.colorBlendOp=VK_BLEND_OP_ADD;
        attachment.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE;attachment.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;attachment.alphaBlendOp=VK_BLEND_OP_ADD;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};cb.attachmentCount=1;cb.pAttachments=&attachment;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};gp.stageCount=2;gp.pStages=stages.data();gp.pVertexInputState=&vi;gp.pInputAssemblyState=&ia;gp.pViewportState=&vp;gp.pDynamicState=&dy;gp.pRasterizationState=&rs;gp.pMultisampleState=&ms;gp.pDepthStencilState=&ds;gp.pColorBlendState=&cb;gp.layout=layout_;gp.renderPass=pass;
        check(vkCreateGraphicsPipelines(device_,VK_NULL_HANDLE,1,&gp,nullptr,&pipeline_),"UI graphics pipeline");
    }catch(...){if(vs)vkDestroyShaderModule(device_,vs,nullptr);if(fs)vkDestroyShaderModule(device_,fs,nullptr);throw;}
    vkDestroyShaderModule(device_,vs,nullptr);vkDestroyShaderModule(device_,fs,nullptr);
}
void VulkanUiPipeline::prepare(const VulkanFrameRuntime::Frame& frame,const UiBatch& ui){
    if(uploadSlot_==UINT32_MAX){atlas_.recordUpload(frame.cmd);uiSet_=atlas_.createDescriptorSet(setLayout_);uploadSlot_=frame.frameSlot;}
    else if(!released_ && uploadSlot_==frame.frameSlot){atlas_.releaseStagingAfterCompletion();released_=true;}
    if(ui.vertices.empty())return;
    auto& buffer=buffers_.at(frame.frameSlot);
    const VkDeviceSize bytes=ui.vertices.size()*sizeof(UiVertex);
    if(buffer.size()<bytes){buffer.destroy();buffer.create(physical_,device_,std::max<VkDeviceSize>(bytes,1024*1024),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);}
    buffer.write(0,ui.vertices.data(),bytes);
}
void VulkanUiPipeline::draw(const VulkanFrameRuntime::Frame& frame,const UiBatch& ui,VkDescriptorSet blocks)const{
    if(ui.vertices.empty())return;
    const VkViewport vp{0,0,float(frame.extent.width),float(frame.extent.height),0,1};const VkRect2D sc{{0,0},frame.extent};
    vkCmdSetViewport(frame.cmd,0,1,&vp);vkCmdSetScissor(frame.cmd,0,1,&sc);
    vkCmdBindPipeline(frame.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_);
    const VkBuffer buffer=buffers_[frame.frameSlot].buffer();const VkDeviceSize zero=0;vkCmdBindVertexBuffers(frame.cmd,0,1,&buffer,&zero);
    VkDescriptorSet active=VK_NULL_HANDLE;
    for(const auto& range:ui.ranges){const auto set=range.blocks?blocks:uiSet_;if(set!=active){vkCmdBindDescriptorSets(frame.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout_,0,1,&set,0,nullptr);active=set;}vkCmdDraw(frame.cmd,range.count,1,range.first,0);}
}
void VulkanUiPipeline::shutdown()noexcept{
    if(!device_)return;
    for(auto& b:buffers_)b.destroy();atlas_.shutdown();
    if(pipeline_)vkDestroyPipeline(device_,pipeline_,nullptr);if(layout_)vkDestroyPipelineLayout(device_,layout_,nullptr);if(setLayout_)vkDestroyDescriptorSetLayout(device_,setLayout_,nullptr);
    pipeline_=VK_NULL_HANDLE;layout_=VK_NULL_HANDLE;setLayout_=VK_NULL_HANDLE;uiSet_=VK_NULL_HANDLE;device_=VK_NULL_HANDLE;physical_=VK_NULL_HANDLE;uploadSlot_=UINT32_MAX;released_=false;
}
}
