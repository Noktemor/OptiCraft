#include "rendering/vulkan/VulkanItemPipeline.h"
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
    check(vkCreateShaderModule(device,&ci,nullptr,&shader),"Item shader");return shader;
}
}
void VulkanItemPipeline::initialize(VulkanFrameRuntime& rt,VkDescriptorSetLayout atlasLayout,std::span<const uint32_t> vs,std::span<const uint32_t> fs){
    initialize(rt.physicalDevice(),rt.device(),rt.renderPass(),atlasLayout,vs,fs);
}
void VulkanItemPipeline::initialize(VkPhysicalDevice physical,VkDevice device,VkRenderPass renderPass,VkDescriptorSetLayout atlasLayout,std::span<const uint32_t> vs,std::span<const uint32_t> fs){
    if(device_||!physical||!device||!renderPass)throw std::invalid_argument("Invalid graphics pipeline device");
    device_=device;physical_=physical;
    try {
        VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(WorldViewParams)};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount=1;pl.pSetLayouts=&atlasLayout;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&push;
        check(vkCreatePipelineLayout(device_,&pl,nullptr,&layout_),"Item pipeline layout");
        recreate(renderPass,vs,fs);
    }catch(...){shutdown();throw;}
}
void VulkanItemPipeline::recreate(VkRenderPass pass,std::span<const uint32_t> vsCode,std::span<const uint32_t> fsCode){
    if(pipeline_)vkDestroyPipeline(device_,pipeline_,nullptr);
    pipeline_=VK_NULL_HANDLE;
    VkShaderModule vs=VK_NULL_HANDLE,fs=VK_NULL_HANDLE;
    try {
        vs=makeShader(device_,vsCode);fs=makeShader(device_,fsCode);
        std::array<VkPipelineShaderStageCreateInfo,2> stages{};
        for(auto& s:stages){s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;s.pName="main";}
        stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vs;
        stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fs;
        const VkVertexInputBindingDescription binding{0,sizeof(ItemVertex),VK_VERTEX_INPUT_RATE_VERTEX};
        const std::array<VkVertexInputAttributeDescription,4> attributes{{
            {0,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(ItemVertex,x)},
            {1,0,VK_FORMAT_R32G32_SFLOAT,offsetof(ItemVertex,u)},
            {2,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(ItemVertex,nx)},
            {3,0,VK_FORMAT_R32_SFLOAT,offsetof(ItemVertex,light)}}};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount=1;vi.pVertexBindingDescriptions=&binding;vi.vertexAttributeDescriptionCount=uint32_t(attributes.size());vi.pVertexAttributeDescriptions=attributes.data();
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=1;vp.scissorCount=1;
        const std::array<VkDynamicState,2> states{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dy.dynamicStateCount=2;dy.pDynamicStates=states.data();
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;rs.lineWidth=1.f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable=VK_TRUE;ds.depthWriteEnable=VK_TRUE;ds.depthCompareOp=VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState attachment{};
        attachment.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
        attachment.blendEnable=VK_FALSE;attachment.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;attachment.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;attachment.colorBlendOp=VK_BLEND_OP_ADD;
        attachment.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE;attachment.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;attachment.alphaBlendOp=VK_BLEND_OP_ADD;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};cb.attachmentCount=1;cb.pAttachments=&attachment;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};gp.stageCount=2;gp.pStages=stages.data();gp.pVertexInputState=&vi;gp.pInputAssemblyState=&ia;gp.pViewportState=&vp;gp.pDynamicState=&dy;gp.pRasterizationState=&rs;gp.pMultisampleState=&ms;gp.pDepthStencilState=&ds;gp.pColorBlendState=&cb;gp.layout=layout_;gp.renderPass=pass;
        check(vkCreateGraphicsPipelines(device_,VK_NULL_HANDLE,1,&gp,nullptr,&pipeline_),"Item graphics pipeline");
    }catch(...){if(vs)vkDestroyShaderModule(device_,vs,nullptr);if(fs)vkDestroyShaderModule(device_,fs,nullptr);throw;}
    vkDestroyShaderModule(device_,vs,nullptr);vkDestroyShaderModule(device_,fs,nullptr);
}
void VulkanItemPipeline::prepare(const VulkanFrameRuntime::Frame& frame,std::span<const ItemVertex> items){
    auto& f=buffers_.at(frame.frameSlot);
    f.count=0;f.indexed=false;stats_={};
    if(items.empty())return;
    if(items.size()>UINT32_MAX)throw std::length_error("Item vertex count");
    if(items.size()%3==0){scratch_.build(items);f.indexed=scratch_.savesBytes();}
    const auto vertices=f.indexed?std::span<const ItemVertex>(scratch_.vertices):items;
    auto upload=[&](VulkanDeviceBuffer& buffer,const void* data,VkDeviceSize bytes,VkBufferUsageFlags usage){
        if(buffer.size()<bytes){buffer.destroy();buffer.create(physical_,device_,std::max<VkDeviceSize>(bytes,256*1024),usage,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);}
        buffer.write(0,data,bytes);
    };
    upload(f.vertices,vertices.data(),vertices.size_bytes(),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    if(f.indexed)upload(f.indices,scratch_.indices.data(),scratch_.indices.size()*sizeof(uint32_t),VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    f.count=uint32_t(items.size());
    stats_={items.size_bytes(),vertices.size_bytes()+(f.indexed?scratch_.indices.size()*sizeof(uint32_t):0),f.indexed};
}
void VulkanItemPipeline::draw(const VulkanFrameRuntime::Frame& frame,VkDescriptorSet atlas,const WorldViewParams& view)const{
    const auto& f=buffers_.at(frame.frameSlot);if(!f.count)return;
    const VkViewport vp{0,0,float(frame.extent.width),float(frame.extent.height),0,1};const VkRect2D sc{{0,0},frame.extent};
    vkCmdSetViewport(frame.cmd,0,1,&vp);vkCmdSetScissor(frame.cmd,0,1,&sc);
    vkCmdBindPipeline(frame.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_);
    const VkBuffer buffer=f.vertices.buffer();const VkDeviceSize zero=0;vkCmdBindVertexBuffers(frame.cmd,0,1,&buffer,&zero);
    vkCmdBindDescriptorSets(frame.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout_,0,1,&atlas,0,nullptr);
    vkCmdPushConstants(frame.cmd,layout_,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(view),&view);
    if(f.indexed){vkCmdBindIndexBuffer(frame.cmd,f.indices.buffer(),0,VK_INDEX_TYPE_UINT32);vkCmdDrawIndexed(frame.cmd,f.count,1,0,0,0);}
    else vkCmdDraw(frame.cmd,f.count,1,0,0);
}
void VulkanItemPipeline::shutdown()noexcept{
    if(!device_)return;
    stats_={};
    for(auto& b:buffers_){b.vertices.destroy();b.indices.destroy();b.count=0;b.indexed=false;}
    if(pipeline_)vkDestroyPipeline(device_,pipeline_,nullptr);
    if(layout_)vkDestroyPipelineLayout(device_,layout_,nullptr);
    pipeline_=VK_NULL_HANDLE;layout_=VK_NULL_HANDLE;device_=VK_NULL_HANDLE;physical_=VK_NULL_HANDLE;
}
}
