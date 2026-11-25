#include "rendering/vulkan/VulkanOpaqueMaterialPipeline.h"
#include "rendering/vulkan/WorldViewParams.h"
#include <array>
#include <cstdlib>
#include <string_view>
#include <stdexcept>
#include <string>

namespace OptiCraft::Vulkan {
namespace {
void checked(VkResult result,const char* label){
    if(result!=VK_SUCCESS)throw std::runtime_error(std::string(label)+" VkResult="+std::to_string(result));
}
VkShaderModule module(VkDevice device,std::span<const uint32_t> code){
    if(code.empty()||code.front()!=0x07230203u)throw std::invalid_argument("Missing SPIR-V");
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize=code.size_bytes();info.pCode=code.data();
    VkShaderModule output=VK_NULL_HANDLE;
    checked(vkCreateShaderModule(device,&info,nullptr,&output),"vkCreateShaderModule");
    return output;
}
}
void VulkanOpaqueMaterialPipeline::initialize(VkDevice device,VkRenderPass pass,
    VkDescriptorSetLayout atlasLayout,const VulkanChunkStreamGpu& stream,
    const VulkanChunkMeshing& mesh,const VulkanMaterialTables& materials,
    std::span<const uint32_t> vertex,std::span<const uint32_t> fragment){
    if(device_||!device||!pass||!atlasLayout||!stream.opaqueSlots()||!mesh.opaqueFaces())
        throw std::invalid_argument("VulkanOpaqueMaterialPipeline invalid resources");
    device_=device;
    VkShaderModule vs=VK_NULL_HANDLE,fs=VK_NULL_HANDLE;
    try{
        std::array<VkDescriptorSetLayoutBinding,7> bindings{};
        for(uint32_t i=0;i<bindings.size();++i){
            bindings[i].binding=i;bindings[i].descriptorCount=1;
            bindings[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].stageFlags=i==6?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT;
            if(i==0 || i>=2)bindings[i].stageFlags|=VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dl.bindingCount=uint32_t(bindings.size());dl.pBindings=bindings.data();
        checked(vkCreateDescriptorSetLayout(device,&dl,nullptr,&meshLayout_),"opaque material layout");
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,uint32_t(bindings.size())*FrameVoxelDescriptors::kFrames};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets=FrameVoxelDescriptors::kFrames;poolInfo.poolSizeCount=1;poolInfo.pPoolSizes=&poolSize;
        checked(vkCreateDescriptorPool(device,&poolInfo,nullptr,&pool_),"opaque material pool");
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        const std::array<VkDescriptorSetLayout,2> frameLayouts{meshLayout_,meshLayout_};
        alloc.descriptorPool=pool_;alloc.descriptorSetCount=FrameVoxelDescriptors::kFrames;alloc.pSetLayouts=frameLayouts.data();
        checked(vkAllocateDescriptorSets(device,&alloc,frameDescriptors_.sets.data()),"opaque material descriptor");
        descriptor_=frameDescriptors_.sets[0];
        const std::array<VkBuffer,7> buffers{stream.opaqueSlots(),mesh.opaqueFaces(),
            materials.blockDescriptors(),materials.faceUv(),stream.metadata(),stream.lights(),stream.blocks()};
        const std::array<VkDeviceSize,7> sizes{
            VkDeviceSize(stream.plan().capacity())*sizeof(GpuChunkSlot),
            VkDeviceSize(mesh.opaqueFaceBytes()),materials.blockDescriptorBytes(),
            materials.faceUvBytes(),stream.voxelSizes()[1],stream.voxelSizes()[2],stream.voxelSizes()[0]};
        std::array<VkDescriptorBufferInfo,7> infos{};
        std::array<VkWriteDescriptorSet,7> writes{};
        for(uint32_t i=0;i<writes.size();++i){
            infos[i]={buffers[i],0,sizes[i]};
            writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet=descriptor_;writes[i].dstBinding=i;
            writes[i].descriptorCount=1;
            writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo=&infos[i];
        }
        for(auto set:frameDescriptors_.sets){
            for(auto& w:writes)w.dstSet=set;
            vkUpdateDescriptorSets(device,uint32_t(writes.size()),writes.data(),0,nullptr);
        }
        const std::array<VkDescriptorSetLayout,2> layouts{meshLayout_,atlasLayout};
        VkPushConstantRange camera{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(WorldViewParams)};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount=2;pl.pSetLayouts=layouts.data();
        pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&camera;
        checked(vkCreatePipelineLayout(device,&pl,nullptr,&layout_),"opaque material pipeline layout");
        vs=module(device,vertex);fs=module(device,fragment);
        std::array<VkPipelineShaderStageCreateInfo,2> stages{};
        for(auto& stage:stages){stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};stage.pName="main";}
        stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vs;
        stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fs;
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount=1;vp.scissorCount=1;
        const std::array<VkDynamicState,2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamics{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamics.dynamicStateCount=2;dynamics.pDynamicStates=dynamicStates.data();
        VkPipelineRasterizationStateCreateInfo rast{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        // All six packed face windings point outward. VulkanGameMirror flips
        // projection Y and the live renderer uses a positive-height viewport.
        const char* disableCull=std::getenv("OPTICRAFT_VULKAN_BACKFACE");
        rast.polygonMode=VK_POLYGON_MODE_FILL;
        rast.cullMode=disableCull && std::string_view(disableCull)=="0"?
            VK_CULL_MODE_NONE:VK_CULL_MODE_BACK_BIT;
        rast.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;rast.lineWidth=1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState ca{};
        ca.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|
            VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount=1;blend.pAttachments=&ca;
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable=VK_TRUE;depth.depthWriteEnable=VK_TRUE;
        depth.depthCompareOp=VK_COMPARE_OP_LESS_OR_EQUAL;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount=2;gp.pStages=stages.data();gp.pVertexInputState=&vi;
        gp.pInputAssemblyState=&ia;gp.pViewportState=&vp;
        gp.pRasterizationState=&rast;gp.pMultisampleState=&ms;
        gp.pDepthStencilState=&depth;gp.pColorBlendState=&blend;
        gp.pDynamicState=&dynamics;gp.layout=layout_;gp.renderPass=pass;
        checked(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gp,nullptr,&pipeline_),
                "vkCreateGraphicsPipelines(opaque atlas)");
    }catch(...){
        if(vs)vkDestroyShaderModule(device,vs,nullptr);
        if(fs)vkDestroyShaderModule(device,fs,nullptr);
        shutdown();throw;
    }
    vkDestroyShaderModule(device,vs,nullptr);vkDestroyShaderModule(device,fs,nullptr);
}
void VulkanOpaqueMaterialPipeline::shutdown()noexcept{
    if(!device_)return;
    if(pipeline_)vkDestroyPipeline(device_,pipeline_,nullptr);
    if(layout_)vkDestroyPipelineLayout(device_,layout_,nullptr);
    if(pool_)vkDestroyDescriptorPool(device_,pool_,nullptr);
    if(meshLayout_)vkDestroyDescriptorSetLayout(device_,meshLayout_,nullptr);
    pipeline_=VK_NULL_HANDLE;layout_=VK_NULL_HANDLE;pool_=VK_NULL_HANDLE;
    meshLayout_=VK_NULL_HANDLE;descriptor_=VK_NULL_HANDLE;frameDescriptors_.reset();device_=VK_NULL_HANDLE;
}
} // namespace OptiCraft::Vulkan
