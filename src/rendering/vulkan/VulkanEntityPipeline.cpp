#include "rendering/vulkan/VulkanEntityPipeline.h"
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
    check(vkCreateShaderModule(device,&ci,nullptr,&shader),"Entity shader");return shader;
}
}
void VulkanEntityPipeline::initialize(VulkanFrameRuntime& rt,VkDescriptorSetLayout atlasLayout,std::span<const uint32_t> vs,std::span<const uint32_t> fs){
    device_=rt.device();physical_=rt.physicalDevice();
    (void)atlasLayout;
    try {
        VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(physical_,&props);maxPoseBytes_=props.limits.maxStorageBufferRange;
        VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};dl.bindingCount=1;dl.pBindings=&binding;
        check(vkCreateDescriptorSetLayout(device_,&dl,nullptr,&poseLayout_),"Entity pose layout");
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,uint32_t(frames_.size())};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=ps.descriptorCount;pi.poolSizeCount=1;pi.pPoolSizes=&ps;
        check(vkCreateDescriptorPool(device_,&pi,nullptr,&pool_),"Entity pose pool");
        for(auto& f:frames_){VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=pool_;ai.descriptorSetCount=1;ai.pSetLayouts=&poseLayout_;check(vkAllocateDescriptorSets(device_,&ai,&f.set),"Entity pose set");}
        VkDescriptorSetLayoutBinding imageBinding{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
        dl.pBindings=&imageBinding;
        check(vkCreateDescriptorSetLayout(device_,&dl,nullptr,&atlasLayout_),"Entity texture layout");
        const VkDescriptorSetLayout layouts[]{poseLayout_,atlasLayout_};
        VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(WorldViewParams)};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount=2;pl.pSetLayouts=layouts;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&push;
        check(vkCreatePipelineLayout(device_,&pl,nullptr,&layout_),"Entity pipeline layout");
        recreate(rt.renderPass(),vs,fs);
    }catch(...){shutdown();throw;}
}
void VulkanEntityPipeline::recreate(VkRenderPass pass,std::span<const uint32_t> vsCode,std::span<const uint32_t> fsCode){
    if(pipeline_)vkDestroyPipeline(device_,pipeline_,nullptr);pipeline_=VK_NULL_HANDLE;
    VkShaderModule vs=VK_NULL_HANDLE,fs=VK_NULL_HANDLE;
    try {
        vs=makeShader(device_,vsCode);fs=makeShader(device_,fsCode);
        std::array<VkPipelineShaderStageCreateInfo,2> stages{};
        for(auto& s:stages){s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;s.pName="main";}
        stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vs;
        stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fs;
        const VkVertexInputBindingDescription binding{0,sizeof(EntityVertex),VK_VERTEX_INPUT_RATE_VERTEX};
        const std::array<VkVertexInputAttributeDescription,5> attributes{{
            {0,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(EntityVertex,position)},
            {1,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(EntityVertex,normal)},
            {2,0,VK_FORMAT_R32G32_SFLOAT,offsetof(EntityVertex,uv)},
            {3,0,VK_FORMAT_R32G32B32A32_SINT,offsetof(EntityVertex,joints)},
            {4,0,VK_FORMAT_R32G32B32A32_SFLOAT,offsetof(EntityVertex,weights)}}};
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
        check(vkCreateGraphicsPipelines(device_,VK_NULL_HANDLE,1,&gp,nullptr,&pipeline_),"Entity graphics pipeline");
    }catch(...){if(vs)vkDestroyShaderModule(device_,vs,nullptr);if(fs)vkDestroyShaderModule(device_,fs,nullptr);throw;}
    vkDestroyShaderModule(device_,vs,nullptr);vkDestroyShaderModule(device_,fs,nullptr);
}
void VulkanEntityPipeline::prepare(const VulkanFrameRuntime::Frame& frame,std::span<const EntityDraw> entities){
    // Keep staging until its frame fence completes, not merely until command recording ends.
    for(auto& [key,a]:assets_)if(a.uploadSlot==frame.frameSlot){for(auto& texture:a.textures)texture->releaseStagingAfterCompletion();a.uploadSlot=UINT32_MAX;}
    if(entities.empty())return;
    const VkDeviceSize bytes=VkDeviceSize(entities.size())*sizeof(EntityPose);
    if(bytes>maxPoseBytes_)throw std::runtime_error("Entity poses exceed storage range");
    poses_.clear();poses_.reserve(entities.size());
    for(const auto& entity:entities){
        poses_.push_back(entity.pose);
        if(!entity.asset || entity.asset->vertices.empty() || entity.asset->indices.empty())continue;
        if(assets_.contains(entity.asset.get()))continue;
        auto& gpu=assets_[entity.asset.get()];gpu.owner=entity.asset;
        auto& asset=*entity.asset;
        const auto flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        const auto preferred=VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        gpu.vertices.create(physical_,device_,asset.vertices.size()*sizeof(EntityVertex),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,flags,preferred);
        gpu.vertices.write(0,asset.vertices.data(),gpu.vertices.size());
        gpu.indices.create(physical_,device_,asset.indices.size()*sizeof(uint32_t),VK_BUFFER_USAGE_INDEX_BUFFER_BIT,flags,preferred);
        gpu.indices.write(0,asset.indices.data(),gpu.indices.size());
        for(const auto& source:asset.textures){
            if(source.rgba.size()!=uint64_t(source.width)*source.height*4)throw std::runtime_error("Invalid entity texture");
            auto tex=std::make_unique<VulkanBlockAtlas>();tex->initializePixels(physical_,device_,source.width,source.height,source.rgba.data(),1,true);
            gpu.sets.push_back(tex->createDescriptorSet(atlasLayout_));tex->recordUpload(frame.cmd);gpu.textures.push_back(std::move(tex));
        }
        gpu.uploadSlot=frame.frameSlot;
    }
    auto& f=frames_.at(frame.frameSlot);
    if(f.buffer.size()<bytes){
        f.buffer.destroy();f.buffer.create(physical_,device_,std::min(maxPoseBytes_,std::max<VkDeviceSize>(bytes,256*1024)),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDescriptorBufferInfo info{f.buffer.buffer(),0,f.buffer.size()};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=f.set;w.dstBinding=0;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w.pBufferInfo=&info;
        vkUpdateDescriptorSets(device_,1,&w,0,nullptr);
    }
    f.buffer.write(0,poses_.data(),bytes);
}
void VulkanEntityPipeline::draw(const VulkanFrameRuntime::Frame& frame,std::span<const EntityDraw> entities,const WorldViewParams& view)const{
    if(entities.empty())return;
    const VkViewport vp{0,0,float(frame.extent.width),float(frame.extent.height),0,1};const VkRect2D sc{{0,0},frame.extent};
    vkCmdSetViewport(frame.cmd,0,1,&vp);vkCmdSetScissor(frame.cmd,0,1,&sc);
    vkCmdBindPipeline(frame.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_);
    const auto poseSet=frames_[frame.frameSlot].set;
    vkCmdBindDescriptorSets(frame.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout_,0,1,&poseSet,0,nullptr);
    vkCmdPushConstants(frame.cmd,layout_,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(view),&view);
    const EntityAsset* active=nullptr;VkDescriptorSet texture=VK_NULL_HANDLE;
    for(uint32_t i=0;i<entities.size();++i){
        const auto& e=entities[i];auto it=assets_.find(e.asset.get());if(it==assets_.end())continue;
        const auto& g=it->second;
        if(active!=e.asset.get()){
            active=e.asset.get();const auto vb=g.vertices.buffer();const VkDeviceSize zero=0;
            vkCmdBindVertexBuffers(frame.cmd,0,1,&vb,&zero);vkCmdBindIndexBuffer(frame.cmd,g.indices.buffer(),0,VK_INDEX_TYPE_UINT32);
        }
        for(const auto& p:e.asset->primitives){
            if(!e.hiddenMaterial.empty() && p.material.find(e.hiddenMaterial)!=std::string::npos)continue;
            if(p.texture>=g.sets.size() || uint64_t(p.first)+p.count>e.asset->indices.size())continue;
            const auto set=g.sets[p.texture];if(set!=texture){texture=set;vkCmdBindDescriptorSets(frame.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout_,1,1,&texture,0,nullptr);}
            vkCmdDrawIndexed(frame.cmd,p.count,1,p.first,0,i);
        }
    }
}
void VulkanEntityPipeline::shutdown()noexcept{
    if(!device_)return;
    assets_.clear();poses_.clear();
    for(auto& f:frames_){f.buffer.destroy();f.set=VK_NULL_HANDLE;}
    if(pipeline_)vkDestroyPipeline(device_,pipeline_,nullptr);
    if(layout_)vkDestroyPipelineLayout(device_,layout_,nullptr);
    if(pool_)vkDestroyDescriptorPool(device_,pool_,nullptr);
    if(poseLayout_)vkDestroyDescriptorSetLayout(device_,poseLayout_,nullptr);
    if(atlasLayout_)vkDestroyDescriptorSetLayout(device_,atlasLayout_,nullptr);
    pipeline_=VK_NULL_HANDLE;layout_=VK_NULL_HANDLE;pool_=VK_NULL_HANDLE;poseLayout_=VK_NULL_HANDLE;atlasLayout_=VK_NULL_HANDLE;device_=VK_NULL_HANDLE;physical_=VK_NULL_HANDLE;
}
}
