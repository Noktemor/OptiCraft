#include "rendering/vulkan/VulkanMaterialWorldgen.h"
#include "rendering/vulkan/VulkanFrameRuntime.h"
#include "world/generation/World_Generator.h"
#include "world/blocks/Block_Types.h"
#include "utils/configs/Config_World.h"
#include "utils/logging/Logger.h"
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
namespace OptiCraft::Vulkan {
namespace {
void check(VkResult r,const char* msg){if(r!=VK_SUCCESS)throw std::runtime_error(std::string(msg)+": "+std::to_string(r));}
constexpr size_t voxels=Config::chunk_size*Config::chunk_size*Config::chunk_height;
static_assert(voxels==32768 && Config::chunk_size==32);
struct Push {int32_t x,z,count,minY,maxY;uint32_t ids[10];};
static_assert(sizeof(Push)==60);
}
VulkanMaterialWorldgen::~VulkanMaterialWorldgen(){
    auto d=runtime_.device();
    if(submitted_)vkWaitForFences(d,1,&fence_,VK_TRUE,UINT64_MAX);
    if(timestamps_)vkDestroyQueryPool(d,timestamps_,nullptr);
    if(fence_)vkDestroyFence(d,fence_,nullptr);
    if(pool_)vkDestroyCommandPool(d,pool_,nullptr);
    if(pipeline_)vkDestroyPipeline(d,pipeline_,nullptr);
    if(pipelineLayout_)vkDestroyPipelineLayout(d,pipelineLayout_,nullptr);
    if(descriptors_)vkDestroyDescriptorPool(d,descriptors_,nullptr);
    if(layout_)vkDestroyDescriptorSetLayout(d,layout_,nullptr);
}
void VulkanMaterialWorldgen::initialize(const World_Generator& gen){
    if(!runtime_.supportsShaderFloat64())throw std::runtime_error("shaderFloat64 unavailable");
    auto d=runtime_.device();auto p=runtime_.physicalDevice();
    VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(p,&props);
    if(props.limits.maxPerStageDescriptorStorageBuffers<7 || props.limits.maxDescriptorSetStorageBuffers<7)
        throw std::runtime_error("insufficient worldgen SSBO bindings");
    Gpu_Noise_Tables tables;
    if(!gen.export_gpu_noise_tables(tables)||tables.nodes.empty())throw std::runtime_error("worldgen noise tables unavailable");
    const std::array<VkDeviceSize,7> sizes{sizeof(tables.permutation),sizeof(tables.gradients),tables.nodes.size()*sizeof(Gpu_Lattice_Node),sizeof(tables.starts),1024*sizeof(Gpu_Material_Column),9*sizeof(int32_t),9*voxels*sizeof(uint32_t)};
    std::array<VkDescriptorSetLayoutBinding,7> bindings{};
    for(uint32_t i=0;i<7;++i){
        if(sizes[i]>props.limits.maxStorageBufferRange)throw std::runtime_error("worldgen SSBO exceeds device limit");
        buffers_[i].create(p,d,sizes[i],VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        bindings[i]={i,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    }
    buffers_[0].write(0,tables.permutation.data(),sizes[0]);buffers_[1].write(0,tables.gradients.data(),sizes[1]);buffers_[2].write(0,tables.nodes.data(),sizes[2]);buffers_[3].write(0,tables.starts.data(),sizes[3]);
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};li.bindingCount=7;li.pBindings=bindings.data();check(vkCreateDescriptorSetLayout(d,&li,nullptr,&layout_),"worldgen descriptor layout");
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,7};VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=1;pi.poolSizeCount=1;pi.pPoolSizes=&ps;check(vkCreateDescriptorPool(d,&pi,nullptr,&descriptors_),"worldgen pool");
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=descriptors_;ai.descriptorSetCount=1;ai.pSetLayouts=&layout_;check(vkAllocateDescriptorSets(d,&ai,&set_),"worldgen descriptor set");
    std::array<VkDescriptorBufferInfo,7> info{};std::array<VkWriteDescriptorSet,7> writes{};
    for(uint32_t i=0;i<7;++i){info[i]={buffers_[i].buffer(),0,sizes[i]};writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=set_;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[i].pBufferInfo=&info[i];}
    vkUpdateDescriptorSets(d,7,writes.data(),0,nullptr);
    VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(Push)};VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&layout_;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&pc;check(vkCreatePipelineLayout(d,&pl,nullptr,&pipelineLayout_),"worldgen pipeline layout");
    std::filesystem::path path="assets/vulkan_shaders/worldgen_material.comp.spv";
#ifdef OPTICRAFT_VULKAN_SHADER_DIR
    if(!std::filesystem::exists(path))path=std::filesystem::path(OPTICRAFT_VULKAN_SHADER_DIR)/"worldgen_material.comp.spv";
#endif
    std::ifstream in(path,std::ios::binary|std::ios::ate);auto len=in.tellg();if(!in||len<=0||static_cast<size_t>(len)%4)throw std::runtime_error("worldgen SPIR-V missing");
    std::vector<uint32_t> spv(static_cast<size_t>(len)/4);in.seekg(0);if(!in.read(reinterpret_cast<char*>(spv.data()),static_cast<std::streamsize>(len)))throw std::runtime_error("worldgen SPIR-V read failed");
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=spv.size()*4;sm.pCode=spv.data();VkShaderModule module=VK_NULL_HANDLE;check(vkCreateShaderModule(d,&sm,nullptr,&module),"worldgen shader");
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.layout=pipelineLayout_;ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=module;ci.stage.pName="main";auto result=vkCreateComputePipelines(d,VK_NULL_HANDLE,1,&ci,nullptr,&pipeline_);vkDestroyShaderModule(d,module,nullptr);check(result,"worldgen compute pipeline");
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;cp.queueFamilyIndex=runtime_.graphicsQueueFamily();check(vkCreateCommandPool(d,&cp,nullptr,&pool_),"worldgen command pool");
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=pool_;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;check(vkAllocateCommandBuffers(d,&ca,&cmd_),"worldgen command buffer");
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};check(vkCreateFence(d,&fi,nullptr,&fence_),"worldgen fence");
    uint32_t n=0;vkGetPhysicalDeviceQueueFamilyProperties(p,&n,nullptr);std::vector<VkQueueFamilyProperties> families(n);vkGetPhysicalDeviceQueueFamilyProperties(p,&n,families.data());timestampBits_=families.at(runtime_.graphicsQueueFamily()).timestampValidBits;timestampNs_=props.limits.timestampPeriod;
    if(timestampBits_&&props.limits.timestampComputeAndGraphics){VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};qi.queryType=VK_QUERY_TYPE_TIMESTAMP;qi.queryCount=2;check(vkCreateQueryPool(d,&qi,nullptr,&timestamps_),"worldgen timestamps");}
    LOG_INFO("Vulkan worldgen: FP64 material stage initialized");
}
bool VulkanMaterialWorldgen::start(const World_Generator& gen,int x,int z,const std::array<Gpu_Material_Column,1024>& columns,const std::vector<int>& ys){
    if(disabled_||submitted_||ys.empty()||ys.size()>9)return false;
    try{
        if(!pipeline_)initialize(gen);
        buffers_[4].write(0,columns.data(),sizeof(columns));buffers_[5].write(0,ys.data(),ys.size()*sizeof(int));
        Push push{x,z,static_cast<int32_t>(ys.size()),Config::world_min_y,Config::world_max_y,{uint32_t(Block_Types::Stone),uint32_t(Block_Types::Deep_Stone),uint32_t(Block_Types::Rare_Mineral),uint32_t(Block_Types::Diamond_Ore),uint32_t(Block_Types::Gold_Ore),uint32_t(Block_Types::Copper_Ore),uint32_t(Block_Types::Lapis_Ore),uint32_t(Block_Types::Coal_Ore),uint32_t(Block_Types::Iron_Ore),uint32_t(Block_Types::Mossy_Stone)}};
        auto d=runtime_.device();check(vkResetCommandBuffer(cmd_,0),"worldgen reset command");VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;check(vkBeginCommandBuffer(cmd_,&bi),"worldgen begin");
        if(timestamps_){vkCmdResetQueryPool(cmd_,timestamps_,0,2);vkCmdWriteTimestamp(cmd_,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,timestamps_,0);}
        vkCmdBindPipeline(cmd_,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_);vkCmdBindDescriptorSets(cmd_,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout_,0,1,&set_,0,nullptr);vkCmdPushConstants(cmd_,pipelineLayout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);vkCmdDispatch(cmd_,static_cast<uint32_t>((ys.size()*voxels+63)/64),1,1);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(cmd_,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        if(timestamps_)vkCmdWriteTimestamp(cmd_,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,timestamps_,1);
        check(vkEndCommandBuffer(cmd_),"worldgen end");check(vkResetFences(d,1,&fence_),"worldgen reset fence");VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&cmd_;started_=std::chrono::steady_clock::now();check(vkQueueSubmit(runtime_.graphicsQueue(),1,&si,fence_),"worldgen submit");submitted_=true;count_=ys.size();return true;
    }catch(const std::exception& e){LOG_WARN(std::string("Vulkan worldgen CPU fallback: ")+e.what());disabled_=true;return false;}
}
bool VulkanMaterialWorldgen::poll(std::vector<uint32_t>& output,double& ms){
    if(!submitted_||disabled_)return false;
    auto r=vkGetFenceStatus(runtime_.device(),fence_);if(r==VK_NOT_READY)return false;if(r!=VK_SUCCESS){disabled_=true;return false;}submitted_=false;
    try{output.resize(count_*voxels);buffers_[6].read(0,output.data(),output.size()*sizeof(uint32_t));ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started_).count();
        uint64_t ticks[2]{};if(timestamps_&&vkGetQueryPoolResults(runtime_.device(),timestamps_,0,2,sizeof(ticks),ticks,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT)==VK_SUCCESS){uint64_t mask=timestampBits_==64?UINT64_MAX:((uint64_t(1)<<timestampBits_)-1);ms=double((ticks[1]-ticks[0])&mask)*timestampNs_/1e6;}
        return true;
    }catch(...){disabled_=true;output.clear();return false;}
}
void VulkanMaterialWorldgen::abandon() noexcept {disabled_=true;count_=0;}
}
