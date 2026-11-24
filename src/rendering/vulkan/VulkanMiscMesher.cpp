#include "rendering/vulkan/VulkanMiscMesher.h"
#include "rendering/vulkan/VulkanBlockCatalog.h"
#include <algorithm>
#include <stdexcept>
#include <string>

namespace OptiCraft::Vulkan {
namespace {
void check(VkResult result,const char* name){
    if(result!=VK_SUCCESS)throw std::runtime_error(std::string("VulkanMiscMesher: ")+name+
        " VkResult="+std::to_string(result));
}
VkShaderModule module(VkDevice device,std::span<const uint32_t> code){
    if(code.empty()||code.front()!=0x07230203u)throw std::invalid_argument("missing SPIR-V");
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize=code.size_bytes();info.pCode=code.data();
    VkShaderModule result=VK_NULL_HANDLE;
    check(vkCreateShaderModule(device,&info,nullptr,&result),"vkCreateShaderModule");
    return result;
}
void barrier(VkCommandBuffer cmd,VkPipelineStageFlags from,VkAccessFlags a,
             VkPipelineStageFlags to,VkAccessFlags b){
    VkMemoryBarrier m{VK_STRUCTURE_TYPE_MEMORY_BARRIER};m.srcAccessMask=a;m.dstAccessMask=b;
    vkCmdPipelineBarrier(cmd,from,to,0,1,&m,0,nullptr,0,nullptr);
}
} // namespace
VulkanMiscMesher::~VulkanMiscMesher(){shutdown();}
void VulkanMiscMesher::initialize(VkPhysicalDevice physical,VkDevice device,
   const GpuMiscBuffers& b,std::span<const uint32_t> countSpv,
   std::span<const uint32_t> emitSpv){
    if(device_ || !device || !physical)throw std::invalid_argument("invalid Vulkan device");
    VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(physical,&props);
    if(props.limits.maxComputeWorkGroupCount[0]<512 ||
       props.limits.maxComputeWorkGroupSize[0]<64 ||
       props.limits.maxComputeWorkGroupInvocations<64 ||
       props.limits.maxPushConstantsSize<8)
        throw std::runtime_error("GPU cannot dispatch 32^3 voxel cells");
    dispatchYLimit_=props.limits.maxComputeWorkGroupCount[1];
    const std::array<VkDeviceSize,11> alignment{
        4,4,sizeof(GpuMiscSlot),sizeof(GpuBlockVisual),sizeof(GpuShapeIndex),
        sizeof(GpuShapeBox),4,32,4,sizeof(GpuFaceUv),4};
    for(size_t i=0;i<11;++i){
        if(!b.buffers[i]||!b.byteSizes[i]||b.byteSizes[i]%alignment[i]||
           b.byteSizes[i]>props.limits.maxStorageBufferRange)
            throw std::invalid_argument("missing/misaligned/oversized GPU SSBO");
    }
    if(b.byteSizes[3]<65536ull*sizeof(GpuBlockVisual) ||
       b.byteSizes[4]<33ull*256ull*sizeof(GpuShapeIndex))
        throw std::invalid_argument("material/shape table incomplete");
    maxJobs_=uint32_t(std::min(b.byteSizes[6]/4,b.byteSizes[8]/4));
    if(!maxJobs_ || !dispatchYLimit_)throw std::invalid_argument("empty work batch");
    device_=device;buffers_=b;
    try{
        std::array<VkDescriptorSetLayoutBinding,11> bindings{};
        for(uint32_t i=0;i<11;i++){
            auto& dst=bindings[i];dst.binding=i;dst.descriptorCount=1;
            dst.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            dst.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT|VK_SHADER_STAGE_VERTEX_BIT;
        }
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dl.bindingCount=uint32_t(bindings.size());dl.pBindings=bindings.data();
        check(vkCreateDescriptorSetLayout(device_,&dl,nullptr,&layout_),"descriptor set layout");
        VkDescriptorPoolSize sz{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,11};
        VkDescriptorPoolCreateInfo pc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pc.poolSizeCount=1;pc.pPoolSizes=&sz;pc.maxSets=1;
        check(vkCreateDescriptorPool(device_,&pc,nullptr,&pool_),"descriptor pool");
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        da.descriptorPool=pool_;da.descriptorSetCount=1;da.pSetLayouts=&layout_;
        check(vkAllocateDescriptorSets(device_,&da,&set_),"descriptor set");
        std::array<VkDescriptorBufferInfo,11> infos{};
        std::array<VkWriteDescriptorSet,11> writes{};
        for(uint32_t i=0;i<11;++i){
            infos[i]={b.buffers[i],0,b.byteSizes[i]};
            auto& w=writes[i];w={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet=set_;w.dstBinding=i;w.descriptorCount=1;
            w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w.pBufferInfo=&infos[i];
        }
        vkUpdateDescriptorSets(device_,uint32_t(writes.size()),writes.data(),0,nullptr);
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,8};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount=1;pl.pSetLayouts=&layout_;pl.pushConstantRangeCount=1;
        pl.pPushConstantRanges=&push;
        check(vkCreatePipelineLayout(device_,&pl,nullptr,&pipelineLayout_),"pipeline layout");
        const std::array<std::span<const uint32_t>,2> programs{countSpv,emitSpv};
        for(size_t i=0;i<2;i++){
            VkShaderModule sh=module(device_,programs[i]);
            VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;stage.module=sh;stage.pName="main";
            VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            cp.stage=stage;cp.layout=pipelineLayout_;
            VkResult res=vkCreateComputePipelines(device_,VK_NULL_HANDLE,1,&cp,nullptr,&pipelines_[i]);
            vkDestroyShaderModule(device_,sh,nullptr);
            check(res,"compute pipeline");
        }
    }catch(...){shutdown();throw;}
}
void VulkanMiscMesher::record(VkCommandBuffer cmd,uint32_t jobs,VkPipeline pipeline)const{
    if(!jobs)return;
    if(!device_||!cmd||jobs>maxJobs_)throw std::out_of_range("invalid misc job batch");
    vkCmdFillBuffer(cmd,buffers_.buffers[6],0,VkDeviceSize(jobs)*4u,0u);
    barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout_,
        0,1,&set_,0,nullptr);
    for(uint32_t first=0;first<jobs;){
        uint32_t n=std::min(dispatchYLimit_,jobs-first);
        const uint32_t pc[]{first,jobs};
        vkCmdPushConstants(cmd,pipelineLayout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(pc),pc);
        vkCmdDispatch(cmd,512,n,1);
        first+=n;
    }
    barrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
        VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_SHADER_READ_BIT);
}
void VulkanMiscMesher::recordCount(VkCommandBuffer cmd,uint32_t jobs)const{
    record(cmd,jobs,pipelines_[0]);
}
void VulkanMiscMesher::recordEmit(VkCommandBuffer cmd,uint32_t jobs)const{
    record(cmd,jobs,pipelines_[1]);
}
void VulkanMiscMesher::shutdown(){
    if(!device_)return;
    for(auto& p:pipelines_) {if(p)vkDestroyPipeline(device_,p,nullptr);p=VK_NULL_HANDLE;}
    if(pipelineLayout_)vkDestroyPipelineLayout(device_,pipelineLayout_,nullptr);
    if(pool_)vkDestroyDescriptorPool(device_,pool_,nullptr);
    if(layout_)vkDestroyDescriptorSetLayout(device_,layout_,nullptr);
    pipelineLayout_=VK_NULL_HANDLE;pool_=VK_NULL_HANDLE;layout_=VK_NULL_HANDLE;
    set_=VK_NULL_HANDLE;device_=VK_NULL_HANDLE;maxJobs_=0;
}
} // namespace OptiCraft::Vulkan
