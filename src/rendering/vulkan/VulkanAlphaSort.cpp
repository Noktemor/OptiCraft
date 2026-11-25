#include "rendering/vulkan/VulkanAlphaSort.h"
#include <stdexcept>
#include <cstdlib>
#include <string_view>
namespace OptiCraft::Vulkan {
namespace {void check(VkResult r){if(r!=VK_SUCCESS)throw std::runtime_error("Vulkan alpha sort resource failure");}}
void VulkanAlphaSort::initialize(VulkanFrameRuntime& rt,const std::array<VkBuffer,VulkanFrameRuntime::kFramesInFlight>& input,VkDeviceSize commandBytes,VkBuffer quads,VkDeviceSize quadBytes,VkBuffer materials,VkDeviceSize materialBytes,std::span<const uint32_t> spv){
    device_=rt.device();
    const char* split=std::getenv("OPTICRAFT_VULKAN_SPLIT_MISC");
    splitCutout_=!split || std::string_view(split)!="0";
    try{
        std::array<VkDescriptorSetLayoutBinding,5> bindings{};
        for(uint32_t i=0;i<5;++i){bindings[i]={i,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};}
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};li.bindingCount=5;li.pBindings=bindings.data();check(vkCreateDescriptorSetLayout(device_,&li,nullptr,&layout_));
        VkDescriptorSetLayoutBinding vertexBinding{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr};
        li.bindingCount=1;li.pBindings=&vertexBinding;check(vkCreateDescriptorSetLayout(device_,&li,nullptr,&vertexLayout_));
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,uint32_t(frames_.size()*6)};VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=uint32_t(frames_.size()*2);pi.poolSizeCount=1;pi.pPoolSizes=&ps;check(vkCreateDescriptorPool(device_,&pi,nullptr,&pool_));
        for(uint32_t i=0;i<frames_.size();++i){auto& f=frames_[i];
            f.commands.create(rt.physicalDevice(),device_,commandBytes*2,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            f.indices.create(rt.physicalDevice(),device_,quadBytes/8,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=pool_;ai.descriptorSetCount=1;ai.pSetLayouts=&layout_;check(vkAllocateDescriptorSets(device_,&ai,&f.set));
            std::array<VkDescriptorBufferInfo,5> infos{{{input[i],0,commandBytes},{quads,0,quadBytes},{materials,0,materialBytes},{f.commands.buffer(),0,f.commands.size()},{f.indices.buffer(),0,f.indices.size()}}};
            std::array<VkWriteDescriptorSet,5> writes{};
            for(uint32_t j=0;j<5;++j){writes[j]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[j].dstSet=f.set;writes[j].dstBinding=j;writes[j].descriptorCount=1;writes[j].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[j].pBufferInfo=&infos[j];}
            vkUpdateDescriptorSets(device_,5,writes.data(),0,nullptr);
            ai.pSetLayouts=&vertexLayout_;check(vkAllocateDescriptorSets(device_,&ai,&f.vertexSet));
            VkWriteDescriptorSet orderWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};orderWrite.dstSet=f.vertexSet;orderWrite.dstBinding=0;orderWrite.descriptorCount=1;orderWrite.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;orderWrite.pBufferInfo=&infos[4];vkUpdateDescriptorSets(device_,1,&orderWrite,0,nullptr);
        }
        VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT,0,16};VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&layout_;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&pc;check(vkCreatePipelineLayout(device_,&pl,nullptr,&pipelineLayout_));
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};mi.codeSize=spv.size_bytes();mi.pCode=spv.data();VkShaderModule module=VK_NULL_HANDLE;check(vkCreateShaderModule(device_,&mi,nullptr,&module));
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.layout=pipelineLayout_;ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=module;ci.stage.pName="main";
        auto result=vkCreateComputePipelines(device_,VK_NULL_HANDLE,1,&ci,nullptr,&pipeline_);vkDestroyShaderModule(device_,module,nullptr);check(result);
    }catch(...){shutdown();throw;}
}
void VulkanAlphaSort::record(const VulkanFrameRuntime::Frame& frame,uint32_t live,const std::array<float,4>& dir)const{
    if(!live)return;
    auto cmd=frame.cmd;auto& f=frames_.at(frame.frameSlot);
    VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};before.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;before.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&before,0,nullptr,0,nullptr);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout_,0,1,&f.set,0,nullptr);auto params=dir;params[3]=splitCutout_?1.0f:0.0f;vkCmdPushConstants(cmd,pipelineLayout_,VK_SHADER_STAGE_COMPUTE_BIT,0,16,params.data());vkCmdDispatch(cmd,live,1,1);
    VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};after.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;after.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT|VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,0,1,&after,0,nullptr,0,nullptr);
}
void VulkanAlphaSort::shutdown()noexcept{
    if(!device_)return;
    if(pipeline_)vkDestroyPipeline(device_,pipeline_,nullptr);
    if(pipelineLayout_)vkDestroyPipelineLayout(device_,pipelineLayout_,nullptr);
    if(pool_)vkDestroyDescriptorPool(device_,pool_,nullptr);
    if(layout_)vkDestroyDescriptorSetLayout(device_,layout_,nullptr);
    if(vertexLayout_)vkDestroyDescriptorSetLayout(device_,vertexLayout_,nullptr);
    for(auto& f:frames_){f.commands.destroy();f.indices.destroy();f.set=VK_NULL_HANDLE;f.vertexSet=VK_NULL_HANDLE;}
    pipeline_=VK_NULL_HANDLE;pipelineLayout_=VK_NULL_HANDLE;pool_=VK_NULL_HANDLE;layout_=VK_NULL_HANDLE;vertexLayout_=VK_NULL_HANDLE;device_=VK_NULL_HANDLE;
}
}
