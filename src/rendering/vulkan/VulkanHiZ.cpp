#include "rendering/vulkan/VulkanHiZ.h"
#include <algorithm>
#include <stdexcept>
namespace OptiCraft::Vulkan {
namespace {
void check(VkResult r){if(r!=VK_SUCCESS)throw std::runtime_error("Vulkan Hi-Z resource creation failed");}
uint64_t pyramidSize(VkExtent2D e){
    uint64_t w=(uint64_t(e.width)+7)/8,h=(uint64_t(e.height)+7)/8,n=0;
    while(true){n+=w*h;if(w==1&&h==1)return n;w=(w+1)/2;h=(h+1)/2;}
}
void memoryBarrier(VkCommandBuffer cmd,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkAccessFlags a,VkAccessFlags b){
    VkMemoryBarrier m{VK_STRUCTURE_TYPE_MEMORY_BARRIER};m.srcAccessMask=a;m.dstAccessMask=b;
    vkCmdPipelineBarrier(cmd,src,dst,0,1,&m,0,nullptr,0,nullptr);
}
void depthBarrier(VkCommandBuffer cmd,VkImage image,bool read){
    VkImageMemoryBarrier m{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    m.oldLayout=read?VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    m.newLayout=read?VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    m.srcAccessMask=read?VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT:VK_ACCESS_SHADER_READ_BIT;
    m.dstAccessMask=read?VK_ACCESS_SHADER_READ_BIT:(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    m.srcQueueFamilyIndex=m.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;m.image=image;
    m.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
    const auto depthStages=VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    vkCmdPipelineBarrier(cmd,read?depthStages:VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        read?VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT:depthStages,0,0,nullptr,0,nullptr,1,&m);
}
}
void VulkanHiZ::initialize(VulkanFrameRuntime& runtime,
    const std::array<VulkanCullerBuffers,VulkanFrameRuntime::kFramesInFlight>& buffers,
    std::span<const uint32_t> spv,
    const std::array<VkBuffer,VulkanFrameRuntime::kFramesInFlight>& miscCommands,VkBuffer miscQuads,VkDeviceSize miscQuadBytes){
    shutdown();if(!runtime.supportsDepthPyramid())return;
    extent_=runtime.extent();if(!extent_.width||!extent_.height)return;
    VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(runtime.physicalDevice(),&props);
    const auto bytes=pyramidSize(extent_)*sizeof(float);
    if(bytes>props.limits.maxStorageBufferRange||props.limits.maxPerStageDescriptorStorageBuffers<8||props.limits.maxDescriptorSetStorageBuffers<8||
       props.limits.maxComputeWorkGroupSize[0]<64||props.limits.maxComputeWorkGroupInvocations<64||props.limits.maxPushConstantsSize<sizeof(Params))return;
    maxGroups_=props.limits.maxComputeWorkGroupCount[0];if(!maxGroups_)return;
    device_=runtime.device();
    try{
        std::array<VkDescriptorSetLayoutBinding,9> bindings{};
        for(uint32_t i=0;i<bindings.size();++i){
            bindings[i].binding=i;
            bindings[i].descriptorType=i==0?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount=1;bindings[i].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};li.bindingCount=uint32_t(bindings.size());li.pBindings=bindings.data();
        check(vkCreateDescriptorSetLayout(device_,&li,nullptr,&layout_));
        const uint32_t n=uint32_t(slots_.size());
        const VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,n},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,n*8}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=n;pi.poolSizeCount=2;pi.pPoolSizes=sizes;
        check(vkCreateDescriptorPool(device_,&pi,nullptr,&pool_));
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};si.magFilter=si.minFilter=VK_FILTER_NEAREST;si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        check(vkCreateSampler(device_,&si,nullptr,&sampler_));
        for(uint32_t i=0;i<n;++i){auto& s=slots_[i];s.buffers=buffers[i];
            s.pyramid.create(runtime.physicalDevice(),device_,bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=pool_;ai.descriptorSetCount=1;ai.pSetLayouts=&layout_;
            check(vkAllocateDescriptorSets(device_,&ai,&s.set));
            std::array<VkDescriptorBufferInfo,8> infos{};std::array<VkWriteDescriptorSet,8> writes{};
            infos[0]={s.pyramid.buffer(),0,s.pyramid.size()};
            for(uint32_t j=0;j<5;++j)infos[j+1]={s.buffers.buffers[j],0,s.buffers.byteSizes[j]};
            infos[6]={miscCommands[i],0,s.buffers.byteSizes[3]};
            infos[7]={miscQuads,0,miscQuadBytes};
            for(uint32_t j=0;j<8;++j){writes[j]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[j].dstSet=s.set;writes[j].dstBinding=j+1;writes[j].descriptorCount=1;writes[j].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[j].pBufferInfo=&infos[j];}
            vkUpdateDescriptorSets(device_,uint32_t(writes.size()),writes.data(),0,nullptr);
        }
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(Params)};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&layout_;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&range;
        check(vkCreatePipelineLayout(device_,&pl,nullptr,&pipelineLayout_));
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};mi.codeSize=spv.size_bytes();mi.pCode=spv.data();VkShaderModule module=VK_NULL_HANDLE;
        check(vkCreateShaderModule(device_,&mi,nullptr,&module));
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.layout=pipelineLayout_;ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=module;ci.stage.pName="main";
        const auto result=vkCreateComputePipelines(device_,VK_NULL_HANDLE,1,&ci,nullptr,&pipeline_);vkDestroyShaderModule(device_,module,nullptr);check(result);
    }catch(...){shutdown();} // optional acceleration: original frustum path remains available
}
void VulkanHiZ::record(VulkanFrameRuntime& runtime,const VulkanFrameRuntime::Frame& frame,const WorldViewParams& view,uint32_t count){
    auto& s=slots_.at(frame.frameSlot);const auto cmd=frame.cmd;
    // Frame slot fence has completed: changing this descriptor cannot race older work.
    VkDescriptorImageInfo image{sampler_,runtime.depthView(frame),VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=s.set;write.dstBinding=0;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&image;
    vkUpdateDescriptorSets(device_,1,&write,0,nullptr);
    depthBarrier(cmd,runtime.depthImage(frame),true);
    // The first pass read these same indirect buffers. Complete those reads before overwrite.
    memoryBarrier(cmd,VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_INDIRECT_COMMAND_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT);
    if(s.buffers.compact){vkCmdFillBuffer(cmd,s.buffers.buffers[4],0,4,0);memoryBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);}
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout_,0,1,&s.set,0,nullptr);
    Params p{view.vp,view.eyeAmbient,{extent_.width,extent_.height,count,s.buffers.compact?1u:0u},{}};
    auto dispatch=[&](uint32_t work){for(uint32_t first=0;first<work;){const uint32_t batch=uint32_t(std::min(uint64_t(work-first),uint64_t(maxGroups_)*64));p.op[2]=first;vkCmdPushConstants(cmd,pipelineLayout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(p),&p);vkCmdDispatch(cmd,(batch+63)/64,1,1);first+=batch;}};
    uint32_t w=(extent_.width+7)/8,h=(extent_.height+7)/8;
    for(uint32_t level=0;;++level){p.op[0]=level?1u:0u;p.op[1]=level;dispatch(w*h);
        memoryBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
        if(w==1&&h==1)break;
        w=(w+1)/2;h=(h+1)/2;
    }
    p.op[0]=2;dispatch(count);
    // Preserve the already sorted misc order; zero only proven hidden commands.
    p.op[0]=3;dispatch(count);
    memoryBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
    depthBarrier(cmd,runtime.depthImage(frame),false);
}
void VulkanHiZ::shutdown()noexcept{
    if(!device_)return;
    if(pipeline_)vkDestroyPipeline(device_,pipeline_,nullptr);
    if(pipelineLayout_)vkDestroyPipelineLayout(device_,pipelineLayout_,nullptr);
    if(pool_)vkDestroyDescriptorPool(device_,pool_,nullptr);
    if(layout_)vkDestroyDescriptorSetLayout(device_,layout_,nullptr);
    if(sampler_)vkDestroySampler(device_,sampler_,nullptr);
    for(auto& s:slots_){s.pyramid.destroy();s.set=VK_NULL_HANDLE;}
    pipeline_=VK_NULL_HANDLE;pipelineLayout_=VK_NULL_HANDLE;pool_=VK_NULL_HANDLE;layout_=VK_NULL_HANDLE;sampler_=VK_NULL_HANDLE;device_=VK_NULL_HANDLE;
}
}
