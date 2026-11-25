#include "rendering/vulkan/VulkanMaterialWorldgen.h"
#include "rendering/vulkan/VulkanFrameRuntime.h"
#include "world/generation/World_Generator.h"
#include "world/generation/Gpu_Tree_Overlay_Catalog.h"
#include "world/blocks/Block_Types.h"
#include "utils/configs/Config_World.h"
#include "utils/logging/Logger.h"
#include <filesystem>
#include <atomic>
#include <cstdlib>
#include <string_view>
#include <fstream>
#include <stdexcept>
#include <string>
namespace OptiCraft::Vulkan {
namespace {
void check(VkResult r,const char* msg){if(r!=VK_SUCCESS)throw std::runtime_error(std::string(msg)+": "+std::to_string(r));}
constexpr size_t voxels=Config::chunk_size*Config::chunk_size*Config::chunk_height;
static_assert(voxels==32768 && Config::chunk_size==32);
struct Push {int32_t x,z,count,minY,maxY;uint32_t ids[10];uint32_t certify;};
static_assert(sizeof(Push)==64);
// Unique across runtime recreation/backend switches; zero means no reuse.
uint64_t nextBaseTicket() noexcept {
    static std::atomic<uint64_t> serial{0};
    return ++serial;
}
}
VulkanMaterialWorldgen::~VulkanMaterialWorldgen(){
    auto d=runtime_.device();
    if(submitted_)vkWaitForFences(d,1,&fence_,VK_TRUE,UINT64_MAX);
    if(propagationPipeline_)vkDestroyPipeline(d,propagationPipeline_,nullptr);
    if(overlayPipeline_)vkDestroyPipeline(d,overlayPipeline_,nullptr);
    if(overlayPipelineLayout_)vkDestroyPipelineLayout(d,overlayPipelineLayout_,nullptr);
    if(overlayPool_)vkDestroyDescriptorPool(d,overlayPool_,nullptr);
    if(overlayLayout_)vkDestroyDescriptorSetLayout(d,overlayLayout_,nullptr);
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
    const std::array<VkDeviceSize,7> sizes{sizeof(tables.permutation),sizeof(tables.gradients),tables.nodes.size()*sizeof(Gpu_Lattice_Node),sizeof(tables.starts),1024*sizeof(Gpu_Material_Column),(Gpu_Base_Region::levels_reserved+Gpu_Base_Region::words(9))*sizeof(uint32_t),(9*(voxels/2)+9)*sizeof(uint32_t)};
    std::array<VkDescriptorSetLayoutBinding,7> bindings{};
    for(uint32_t i=0;i<7;++i){
        if(sizes[i]>props.limits.maxStorageBufferRange)throw std::runtime_error("worldgen SSBO exceeds device limit");
        buffers_[i].create(p,d,sizes[i],VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|(i==6?VK_BUFFER_USAGE_TRANSFER_SRC_BIT:0u),VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
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
    initialize_commands();
    LOG_INFO("Vulkan worldgen: FP64 material stage initialized");
}
void VulkanMaterialWorldgen::initialize_commands(){
    if(pool_){
        if(!cmd_||!fence_)throw std::runtime_error("incomplete worldgen command resources");
        return;
    }
    auto d=runtime_.device();auto p=runtime_.physicalDevice();
    VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(p,&props);
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;cp.queueFamilyIndex=runtime_.graphicsQueueFamily();check(vkCreateCommandPool(d,&cp,nullptr,&pool_),"worldgen command pool");
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=pool_;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;check(vkAllocateCommandBuffers(d,&ca,&cmd_),"worldgen command buffer");
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};check(vkCreateFence(d,&fi,nullptr,&fence_),"worldgen fence");
    uint32_t n=0;vkGetPhysicalDeviceQueueFamilyProperties(p,&n,nullptr);std::vector<VkQueueFamilyProperties> families(n);vkGetPhysicalDeviceQueueFamilyProperties(p,&n,families.data());timestampBits_=families.at(runtime_.graphicsQueueFamily()).timestampValidBits;timestampNs_=props.limits.timestampPeriod;
    if(timestampBits_&&props.limits.timestampComputeAndGraphics){VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};qi.queryType=VK_QUERY_TYPE_TIMESTAMP;qi.queryCount=2;check(vkCreateQueryPool(d,&qi,nullptr,&timestamps_),"worldgen timestamps");}
}
bool VulkanMaterialWorldgen::start(const World_Generator& gen,int x,int z,const std::array<Gpu_Material_Column,1024>& columns,const std::vector<int>& ys, const Gpu_Base_Region& region){
    return start_impl(gen,x,z,columns,ys,region,false);
}
bool VulkanMaterialWorldgen::start_base_certificate(const World_Generator& gen,int x,int z,const std::array<Gpu_Material_Column,1024>& columns,const std::vector<int>& ys,const Gpu_Base_Region& region,const Gpu_Cave_Dispatch::Packet* cave){
    if(region.data.empty()||ys.empty()||ys.size()>9)return false;
    for(size_t i=0;i<ys.size();++i)
        for(size_t j=0;j<i;++j)if(ys[i]==ys[j])return false;
    return start_impl(gen,x,z,columns,ys,region,true,cave);
}
bool VulkanMaterialWorldgen::start_impl(const World_Generator& gen,int x,int z,const std::array<Gpu_Material_Column,1024>& columns,const std::vector<int>& ys,const Gpu_Base_Region& region,bool certify,const Gpu_Cave_Dispatch::Packet* cave){
    if(disabled_||submitted_||ys.empty()||ys.size()>9)return false;
    overlayCount_=0; overlaySky_=false; overlaySeeds_=false; overlayPropagated_=false;
    const bool base = !region.data.empty();
    if(base && region.data.size()!=Gpu_Base_Region::words(ys.size()))return false;
    for(const auto& c:columns) if(bool(c.padding & Gpu_Base_Region::enabled)!=base)return false;
    // No overlay submission may be active here (submitted_ guard above).
    // Retire the old native page only after its last consumer fence drained.
    if(nativeColumn_.busy())nativeColumn_.retire();
    nativeBase_=false;
    baseTicket_=0; // a new dispatch may overwrite the retained material output
    baseMode_=base;
    certifiedLevels_={};certifying_=false;
    try{
        if(!pipeline_)initialize(gen);
        buffers_[4].write(0,columns.data(),sizeof(columns));buffers_[5].write(0,ys.data(),ys.size()*sizeof(int));
        if(base)buffers_[5].write(Gpu_Base_Region::levels_reserved*sizeof(uint32_t),region.data.data(),region.data.size()*sizeof(uint32_t));
        Push push{x,z,static_cast<int32_t>(ys.size()),Config::world_min_y,Config::world_max_y,{uint32_t(Block_Types::Stone),uint32_t(Block_Types::Deep_Stone),uint32_t(Block_Types::Rare_Mineral),uint32_t(Block_Types::Diamond_Ore),uint32_t(Block_Types::Gold_Ore),uint32_t(Block_Types::Copper_Ore),uint32_t(Block_Types::Lapis_Ore),uint32_t(Block_Types::Coal_Ore),uint32_t(Block_Types::Iron_Ore),uint32_t(Block_Types::Mossy_Stone)},uint32_t(certify)};
        if(certify){
            for(size_t i=0;i<ys.size();++i)certifiedLevels_[i]={0,uint32_t(i),x,ys[i],z};
        }
        auto d=runtime_.device();check(vkResetCommandBuffer(cmd_,0),"worldgen reset command");VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;check(vkBeginCommandBuffer(cmd_,&bi),"worldgen begin");
        if(timestamps_){vkCmdResetQueryPool(cmd_,timestamps_,0,2);vkCmdWriteTimestamp(cmd_,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,timestamps_,0);}
        nativeBase_=certify&&recordPreparedBase(gen,x,z,ys,push.ids,cave);
        if(!nativeBase_){
            if(certify){
                // Only the legacy path needs a host certificate clear. Native
                // clears its own device-local certificate via vkCmdFillBuffer.
                const std::array<uint32_t,9> zero{};
                buffers_[6].write(ys.size()*(voxels/2)*4,zero.data(),ys.size()*4);
            }
            vkCmdBindPipeline(cmd_,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_);vkCmdBindDescriptorSets(cmd_,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout_,0,1,&set_,0,nullptr);vkCmdPushConstants(cmd_,pipelineLayout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);vkCmdDispatch(cmd_,static_cast<uint32_t>((ys.size()*voxels+63)/64),1,1);
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(cmd_,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        }
        if(timestamps_)vkCmdWriteTimestamp(cmd_,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,timestamps_,1);
        check(vkEndCommandBuffer(cmd_),"worldgen end");check(vkResetFences(d,1,&fence_),"worldgen reset fence");VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&cmd_;started_=std::chrono::steady_clock::now();check(vkQueueSubmit(runtime_.graphicsQueue(),1,&si,fence_),"worldgen submit");submitted_=true;count_=ys.size();certifying_=certify;return true;
    }catch(const std::exception& e){LOG_WARN(std::string("Vulkan worldgen CPU fallback: ")+e.what());disabled_=true;return false;}
}
bool VulkanMaterialWorldgen::recordPreparedBase(const World_Generator& gen,int x,int z,std::span<const int32_t> levels,
                                                std::span<const uint32_t,10> ids,const Gpu_Cave_Dispatch::Packet* cave){
    if(nativeUnavailable_)return false;
    try {
        if(!nativeInitialized_){
            std::filesystem::path dir="assets/vulkan_shaders";
#ifdef OPTICRAFT_VULKAN_SHADER_DIR
            if(!std::filesystem::exists(dir/"worldgen_material.comp.spv"))dir=OPTICRAFT_VULKAN_SHADER_DIR;
#endif
            nativeColumn_.initialize(runtime_.physicalDevice(),runtime_.device(),runtime_.supportsShaderFloat64(),dir);
            nativeInitialized_=true;
        }
        using R=VulkanColumnMaterialRuntime;using Resource=R::Resource;
        R::Sources sources{};
        auto bind=[&](Resource r,size_t i,VkDeviceSize bytes){
            sources[size_t(r)]={buffers_[i].buffer(),0,bytes,buffers_[i].size()};
        };
        bind(Resource::OrePermutation,0,buffers_[0].size());
        bind(Resource::OreGradients,1,buffers_[1].size());
        bind(Resource::OreLattice,2,buffers_[2].size());
        bind(Resource::OreOctants,3,buffers_[3].size());
        bind(Resource::Columns,4,1024*sizeof(Gpu_Material_Column));
        bind(Resource::InputWords,5,(Gpu_Base_Region::levels_reserved+Gpu_Base_Region::words(levels.size()))*4);
        // Climate is a separately optional producer. Failure before recording
        // must not disable the established Base-only device-local path.
        if(!climateUnavailable_){
            try{
                Gpu_Layer_Dispatch::Push layer{};
                if(gen.export_gpu_layer_push(x,z,layer)){
                    if(!climateInitialized_){
                        Gpu_Climate_Tables tables;
                        if(!gen.export_gpu_climate_tables(tables))throw std::runtime_error("Climate tables unavailable");
                        std::filesystem::path dir="assets/vulkan_shaders";
#ifdef OPTICRAFT_VULKAN_SHADER_DIR
                        if(!std::filesystem::exists(dir/"worldgen_material.comp.spv"))dir=OPTICRAFT_VULKAN_SHADER_DIR;
#endif
                        nativeColumn_.initializeClimate(tables,dir);climateInitialized_=true;
                    }
                    if(cave&&!caveUnavailable_&&!caveInitialized_){
                        try {
                            std::filesystem::path dir="assets/vulkan_shaders";
#ifdef OPTICRAFT_VULKAN_SHADER_DIR
                            if(!std::filesystem::exists(dir/"worldgen_material.comp.spv"))dir=OPTICRAFT_VULKAN_SHADER_DIR;
#endif
                            nativeColumn_.initializeCave(dir);caveInitialized_=true;
                        }catch(const std::exception& e){caveUnavailable_=true;LOG_WARN(std::string("Vulkan cave fallback: ")+e.what());}
                    }
                    const auto* caveSource=caveInitialized_&&!caveUnavailable_?cave:nullptr;
                    if(!riverUnavailable_){
                        try {
                            using D=Gpu_River_Dispatch;
                            // CPU preparation has already touched the region. Never
                            // start a new priority-flood on the render thread.
                            auto packet=gen.gpu_compact_river_region(D::region_of_chunk(x),D::region_of_chunk(z),true);
                            if(packet){
                                if(!riverInitialized_){
                                    std::filesystem::path dir="assets/vulkan_shaders";
#ifdef OPTICRAFT_VULKAN_SHADER_DIR
                                    if(!std::filesystem::exists(dir/"worldgen_material.comp.spv"))dir=OPTICRAFT_VULKAN_SHADER_DIR;
#endif
                                    nativeColumn_.initializeRiver(dir);riverInitialized_=true;
                                }
                                if(nativeColumn_.recordClimateRiverPrepared(cmd_,x,z,levels,Config::world_min_y,Config::world_max_y,ids,
                                      buffers_[2].size()/sizeof(Gpu_Lattice_Node),sources,layer,std::move(packet),caveSource))return true;
                            }
                        }catch(const std::exception& e){riverUnavailable_=true;LOG_WARN(std::string("Vulkan river fallback: ")+e.what());}
                    }
                    if(nativeColumn_.recordClimatePrepared(cmd_,x,z,levels,Config::world_min_y,Config::world_max_y,ids,
                           buffers_[2].size()/sizeof(Gpu_Lattice_Node),sources,layer,caveSource))return true;
                }
            }catch(const std::exception& e){climateUnavailable_=true;LOG_WARN(std::string("Vulkan climate fallback: ")+e.what());}
        }
        return nativeColumn_.recordPrepared(cmd_,x,z,levels,Config::world_min_y,Config::world_max_y,ids,
                                            buffers_[2].size()/sizeof(Gpu_Lattice_Node),sources);
    }catch(const std::exception& e){
        // All throwing resource creation happens before any native commands.
        // A failing queue submission is handled by start_impl, never retried.
        nativeColumn_.shutdown();nativeInitialized_=false;nativeUnavailable_=true;
        LOG_WARN(std::string("Vulkan native column fallback: ")+e.what());return false;
    }
}
bool VulkanMaterialWorldgen::reserveOverlaySnapshot(bool required) {
    // Required snapshot mode is independent of the optional mirror cache toggle.
    const char* mode=std::getenv("OPTICRAFT_VULKAN_WORLDGEN_RESIDENT_REUSE");
    if(!required && mode && std::string_view(mode)=="0")return false;
    auto choice=overlaySnapshots_.end();
    for(auto it=overlaySnapshots_.begin();it!=overlaySnapshots_.end();++it){
        if(!*it){choice=it;break;}
        if(it->use_count()==1 && (choice==overlaySnapshots_.end() || (*it)->ticket<(*choice)->ticket))choice=it;
    }
    if(choice==overlaySnapshots_.end())return false;
    auto& entry=*choice;
    try {
        if(!entry){
            auto fresh=std::make_shared<OverlaySnapshot>();
            fresh->device=runtime_.device();
            fresh->buffer.create(runtime_.physicalDevice(),fresh->device,
                OverlaySnapshot::blockBytes+OverlaySnapshot::metaBytes,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            entry=std::move(fresh);
        }
    } catch(const std::exception&) {return false;}
    entry->ready=false;
    entry->skyRoof={};
    entry->ticket=nextBaseTicket();
    if(!entry->ticket)return false;
    pendingOverlaySnapshot_=entry;
    return true;
}
void VulkanMaterialWorldgen::recordOverlaySnapshot() {
    if(!pendingOverlaySnapshot_)return;
    // Some untouched words were written by the host or the base reuse copy.
    VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    before.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_HOST_WRITE_BIT;
    before.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd_,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&before,0,nullptr,0,nullptr);
    const VkBufferCopy blocks{0,0,OverlaySnapshot::blockBytes};
    const VkBufferCopy meta{0,OverlaySnapshot::blockBytes,OverlaySnapshot::metaBytes};
    vkCmdCopyBuffer(cmd_,overlayBuffers_[0].buffer(),pendingOverlaySnapshot_->buffer.buffer(),1,&blocks);
    vkCmdCopyBuffer(cmd_,overlayBuffers_[1].buffer(),pendingOverlaySnapshot_->buffer.buffer(),1,&meta);
}
void VulkanMaterialWorldgen::initialize_overlay() {
    auto d=runtime_.device(); auto p=runtime_.physicalDevice();
    VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(p,&props);
    std::array<VkDeviceSize,5> sizes{
        Gpu_Overlay_Work::block_words*4, Gpu_Overlay_Work::meta_words*4,
        Generation_Overlay_Packet::group_count*sizeof(Generation_Overlay_Packet::Task),
        (Gpu_Overlay_Work::max_commands+Generation_Sky::seed_input_words)*4,
        (SkyRoofPalette::resultOffset+SkyRoofPalette::resultWords)*4};
    const auto catalog=Gpu_Tree_Overlay::catalog_words();
    const auto plainCommandBytes=sizes[3];
    const VkDeviceSize catalogBytes=(Gpu_Tree_Overlay::range_offset+catalog.size())*4ull+Gpu_Tree_Overlay::max_instances*sizeof(Gpu_Tree_Overlay::Selection);
    if(!catalog.empty() && catalogBytes<=props.limits.maxStorageBufferRange)sizes[3]=catalogBytes;
    std::array<VkDescriptorSetLayoutBinding,5> bindings{};
    for(uint32_t i=0;i<5;++i){
        if(sizes[i]>props.limits.maxStorageBufferRange) throw std::runtime_error("overlay SSBO limit");
        const auto allocate=[&]{overlayBuffers_[i].create(p,d,sizes[i],VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|(i==0?VK_BUFFER_USAGE_TRANSFER_DST_BIT:0u)|(i<2?VK_BUFFER_USAGE_TRANSFER_SRC_BIT:0u),
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT);};
        try {allocate();}
        catch(const std::exception&){
            if(i!=3||sizes[3]==plainCommandBytes)throw;
            sizes[3]=plainCommandBytes;allocate(); // ordinary overlay remains available
        }
        if(i==3 && sizes[3]!=plainCommandBytes){
            overlayBuffers_[3].write(Gpu_Tree_Overlay::range_offset*4ull,catalog.data(),catalog.size_bytes());
            treeCatalogReady_=true;
        }
        bindings[i]={i,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};li.bindingCount=5;li.pBindings=bindings.data();
    check(vkCreateDescriptorSetLayout(d,&li,nullptr,&overlayLayout_),"overlay descriptor layout");
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,5};VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=1;pi.poolSizeCount=1;pi.pPoolSizes=&ps;
    check(vkCreateDescriptorPool(d,&pi,nullptr,&overlayPool_),"overlay descriptor pool");
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=overlayPool_;ai.descriptorSetCount=1;ai.pSetLayouts=&overlayLayout_;
    check(vkAllocateDescriptorSets(d,&ai,&overlaySet_),"overlay descriptor set");
    std::array<VkDescriptorBufferInfo,5> info{};std::array<VkWriteDescriptorSet,5> writes{};
    for(uint32_t i=0;i<5;++i){info[i]={overlayBuffers_[i].buffer(),0,sizes[i]};writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=overlaySet_;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[i].pBufferInfo=&info[i];}
    vkUpdateDescriptorSets(d,5,writes.data(),0,nullptr);
    VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(Generation_Light_Propagation::Params)};VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&overlayLayout_;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&pc;
    check(vkCreatePipelineLayout(d,&pl,nullptr,&overlayPipelineLayout_),"overlay pipeline layout");
    std::filesystem::path path="assets/vulkan_shaders/worldgen_overlay.comp.spv";
#ifdef OPTICRAFT_VULKAN_SHADER_DIR
    if(!std::filesystem::exists(path))path=std::filesystem::path(OPTICRAFT_VULKAN_SHADER_DIR)/"worldgen_overlay.comp.spv";
#endif
    std::ifstream in(path,std::ios::binary|std::ios::ate);auto len=in.tellg();if(!in||len<=0||static_cast<size_t>(len)%4)throw std::runtime_error("overlay SPIR-V missing");
    std::vector<uint32_t> spv(static_cast<size_t>(len)/4);in.seekg(0);if(!in.read(reinterpret_cast<char*>(spv.data()),static_cast<std::streamsize>(len)))throw std::runtime_error("overlay SPIR-V read failed");
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=spv.size()*4;sm.pCode=spv.data();VkShaderModule module=VK_NULL_HANDLE;check(vkCreateShaderModule(d,&sm,nullptr,&module),"overlay shader");
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.layout=overlayPipelineLayout_;ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=module;ci.stage.pName="main";
    auto result=vkCreateComputePipelines(d,VK_NULL_HANDLE,1,&ci,nullptr,&overlayPipeline_);vkDestroyShaderModule(d,module,nullptr);check(result,"overlay pipeline");
}
void VulkanMaterialWorldgen::initialize_propagation() {
    auto d=runtime_.device();
    std::filesystem::path path="assets/vulkan_shaders/worldgen_light_propagate.comp.spv";
#ifdef OPTICRAFT_VULKAN_SHADER_DIR
    if(!std::filesystem::exists(path))path=std::filesystem::path(OPTICRAFT_VULKAN_SHADER_DIR)/"worldgen_light_propagate.comp.spv";
#endif
    std::ifstream in(path,std::ios::binary|std::ios::ate);auto len=in.tellg();if(!in||len<=0||static_cast<size_t>(len)%4)throw std::runtime_error("overlay SPIR-V missing");
    std::vector<uint32_t> spv(static_cast<size_t>(len)/4);in.seekg(0);if(!in.read(reinterpret_cast<char*>(spv.data()),static_cast<std::streamsize>(len)))throw std::runtime_error("overlay SPIR-V read failed");
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=spv.size()*4;sm.pCode=spv.data();VkShaderModule module=VK_NULL_HANDLE;check(vkCreateShaderModule(d,&sm,nullptr,&module),"overlay shader");
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.layout=overlayPipelineLayout_;ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=module;ci.stage.pName="main";
    auto result=vkCreateComputePipelines(d,VK_NULL_HANDLE,1,&ci,nullptr,&propagationPipeline_);vkDestroyShaderModule(d,module,nullptr);check(result,"overlay pipeline");
}
bool VulkanMaterialWorldgen::start_overlay(const Gpu_Overlay_Work& work) {
    return start_overlay_impl(work,false);
}
bool VulkanMaterialWorldgen::start_overlay_snapshot(const Gpu_Overlay_Work& work) {
    if(!work.valid_snapshot_input())return false;
    return start_overlay_impl(work,true);
}
bool VulkanMaterialWorldgen::start_overlay_certified(const Gpu_Overlay_Work& work,const Gpu_Certified_Base& certified) {
    if(!certified.ticket || certified.level>=count_ || certified.level>=certifiedLevels_.size() ||
       certifiedLevels_[certified.level]!=certified || work.level!=certified.level ||
       !work.valid_certified_snapshot_input())return false;
    return start_overlay_impl(work,true,&certified);
}
bool VulkanMaterialWorldgen::start_overlay_impl(const Gpu_Overlay_Work& work,bool snapshotOnly,const Gpu_Certified_Base* certified) {
    if(disabled_||overlayDisabled_||submitted_||(!snapshotOnly&&!pipeline_)||(!certified && work.base.size()!=Gpu_Overlay_Work::block_words)||
       (!snapshotOnly && (work.packet.tasks.empty()||work.packet.commands.empty()))||
       work.packet.tasks.size()>Generation_Overlay_Packet::group_count||work.packet.commands.size()>Gpu_Overlay_Work::max_commands ||
       (!work.tree_selections.empty() && (!certified || !Gpu_Tree_Overlay::valid_selections(work.tree_selections,work.trees))) ||
       !Generation_Sky::valid_input(work.sky_input.size()) ||
       (work.propagate && (!Generation_Sky::has_seeds(work.sky_input.size()) ||
                           work.propagation_passes==0 || work.propagation_passes>Generation_Light_Propagation::max_block_passes))) return false;
    try {
        lastOverlayReference_={};
        pendingOverlaySnapshot_.reset();
        // Reserve before recording or submitting work. A required snapshot
        // cannot silently turn into a host readback when all leases are busy.
        if(!reserveOverlaySnapshot(snapshotOnly) && snapshotOnly)return false;
        if(snapshotOnly)initialize_commands();
        if(!overlayPipeline_)initialize_overlay();
        if(!work.trees.empty()){
            if(!certified){pendingOverlaySnapshot_.reset();return false;}
            if(!treeCatalogReady_){
                pendingOverlaySnapshot_.reset();
                Gpu_Overlay_Work expanded;
                if(!work.expand_tree_snapshot(Gpu_Tree_Overlay::catalog_words(),expanded))return false;
                return start_overlay_impl(expanded,true,certified);
            }
            overlayBuffers_[3].write(Gpu_Tree_Overlay::instance_offset*4ull,work.trees.data(),work.trees.size()*sizeof(Gpu_Tree_Overlay::Instance));
        }
        treeReportCount_=0;
        const uint32_t selectionOffset=work.tree_selections.empty()?0u:uint32_t(Gpu_Tree_Overlay::range_offset+Gpu_Tree_Overlay::catalog_words().size());
        if(!work.tree_selections.empty())overlayBuffers_[3].write(selectionOffset*4ull,work.tree_selections.data(),work.tree_selections.size()*sizeof(Gpu_Tree_Overlay::Selection));
        if(work.propagate && !propagationPipeline_)initialize_propagation();
        static const std::array<uint32_t,Gpu_Overlay_Work::meta_words> zero{};
        const bool reuse=certified || (!snapshotOnly && work.reuse.matches(baseTicket_,count_));
        if(!reuse)overlayBuffers_[0].write(0,work.base.data(),work.base.size()*4);
        overlayBuffers_[1].write(0,zero.data(),sizeof(zero));
        if(!work.packet.tasks.empty()){
            overlayBuffers_[2].write(0,work.packet.tasks.data(),work.packet.tasks.size()*sizeof(Generation_Overlay_Packet::Task));
            overlayBuffers_[3].write(0,work.packet.commands.data(),work.packet.commands.size()*4);
        }
        const bool sky = !work.sky_input.empty();
        const bool seeds = Generation_Sky::has_seeds(work.sky_input.size());
        if(sky) overlayBuffers_[3].write(Gpu_Overlay_Work::max_commands*4,work.sky_input.data(),work.sky_input.size()*4);
        const uint32_t push[3]{static_cast<uint32_t>(work.packet.tasks.size()),0,0};
        auto d=runtime_.device();check(vkResetCommandBuffer(cmd_,0),"overlay reset command");
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;check(vkBeginCommandBuffer(cmd_,&bi),"overlay begin");
        if(timestamps_){vkCmdResetQueryPool(cmd_,timestamps_,0,2);vkCmdWriteTimestamp(cmd_,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,timestamps_,0);}
        if(reuse){
            // Original base completed on this same queue. Explicit dependencies
            // expose the shader output and order overwrites of the overlay page.
            VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            before.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
            before.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd_,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&before,0,nullptr,0,nullptr);
            const VkBufferCopy copy{VkDeviceSize(certified?certified->level:work.reuse.level)*Gpu_Base_Reuse::words*4,0,Gpu_Base_Reuse::words*4};
            const auto source=certified&&nativeBase_?nativeColumn_.result().buffer:buffers_[6].buffer();
            vkCmdCopyBuffer(cmd_,source,overlayBuffers_[0].buffer(),1,&copy);
            if(work.reuse.repair_count){
                VkMemoryBarrier patch{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                patch.srcAccessMask=patch.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
                vkCmdPipelineBarrier(cmd_,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0,1,&patch,0,nullptr,0,nullptr);
                // Patch only words containing CPU-resolved fallback IDs. Merge
                // adjacent words into one vkCmdUpdateBuffer (at most 16 words).
                for(size_t i=0;i<work.reuse.repair_count;){
                    const size_t first=work.reuse.repairs[i];
                    size_t end=i+1;
                    while(end<work.reuse.repair_count && work.reuse.repairs[end]==first+end-i)++end;
                    vkCmdUpdateBuffer(cmd_,overlayBuffers_[0].buffer(),first*4,(end-i)*4,work.base.data()+first);
                    i=end;
                }
            }
            VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            after.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
            after.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd_,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0,1,&after,0,nullptr,0,nullptr);
        }
        if(push[0]){
            vkCmdBindPipeline(cmd_,VK_PIPELINE_BIND_POINT_COMPUTE,overlayPipeline_);
            vkCmdBindDescriptorSets(cmd_,VK_PIPELINE_BIND_POINT_COMPUTE,overlayPipelineLayout_,0,1,&overlaySet_,0,nullptr);
            if(!work.tree_selections.empty()){
                const uint32_t selectionPush[3]{Gpu_Tree_Overlay::selection_mode,selectionOffset,uint32_t(work.tree_selections.size())};
                vkCmdPushConstants(cmd_,overlayPipelineLayout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(selectionPush),selectionPush);
                vkCmdDispatch(cmd_,(selectionPush[2]+63)/64,1,1);
                VkMemoryBarrier selectionBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                selectionBarrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
                selectionBarrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(cmd_,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&selectionBarrier,0,nullptr,0,nullptr);
            }
            vkCmdPushConstants(cmd_,overlayPipelineLayout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),push);
            vkCmdDispatch(cmd_,(push[0]+63)/64,1,1);
        }
        if(sky){
            VkMemoryBarrier dependency{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            dependency.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
            dependency.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd_,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&dependency,0,nullptr,0,nullptr);
            const uint32_t lightPush[3]{Generation_Sky::dispatch_mode,0,0};
            vkCmdPushConstants(cmd_,overlayPipelineLayout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(lightPush),lightPush);
            vkCmdDispatch(cmd_,Generation_Sky::columns/64,1,1);
            if(seeds){
                vkCmdPipelineBarrier(cmd_,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&dependency,0,nullptr,0,nullptr);
                const uint32_t seedPush[3]{Generation_Sky::seed_dispatch_mode,0,0};
                vkCmdPushConstants(cmd_,overlayPipelineLayout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(seedPush),seedPush);
                vkCmdDispatch(cmd_,Generation_Sky::seed_words/64,1,1);
                if(work.propagate){
                    using namespace Generation_Light_Propagation;
                    constexpr uint32_t seedBase=Generation_Overlay_Packet::group_count+Generation_Sky::columns;
                    constexpr uint32_t scratchA=seedBase+light_words, scratchB=scratchA+light_words;
                    constexpr uint32_t transparencyBase=Gpu_Overlay_Work::max_commands+Generation_Sky::columns;
                    vkCmdBindPipeline(cmd_,VK_PIPELINE_BIND_POINT_COMPUTE,propagationPipeline_);
                    for(unsigned pass=0;pass<work.propagation_passes;++pass){
                        // Preserve original seeds for exact CPU validation. A and B never alias.
                        Params params=block_iteration(0,transparencyBase,seedBase,scratchA,scratchB,pass,work.propagation_passes);
                        const auto dispatch=work.light_bounds.dispatch(pass);
                        params.tileOrigin=dispatch.origin;
                        vkCmdPipelineBarrier(cmd_,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&dependency,0,nullptr,0,nullptr);
                        vkCmdPushConstants(cmd_,overlayPipelineLayout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(params),&params);
                        vkCmdDispatch(cmd_,dispatch.x,dispatch.y,dispatch.z);
                    }
                }
            }
        }
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        if(snapshotOnly){
            // Include untouched Base and host-written words in the dependency.
            barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_HOST_WRITE_BIT;
            barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd_,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_HOST_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
            vkCmdBindPipeline(cmd_,VK_PIPELINE_BIND_POINT_COMPUTE,overlayPipeline_);
            vkCmdBindDescriptorSets(cmd_,VK_PIPELINE_BIND_POINT_COMPUTE,overlayPipelineLayout_,0,1,&overlaySet_,0,nullptr);
            const uint32_t roofPush[3]{SkyRoofPalette::mode,0,0};
            vkCmdPushConstants(cmd_,overlayPipelineLayout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(roofPush),roofPush);
            vkCmdDispatch(cmd_,1,1,1);
        }
        recordOverlaySnapshot();
        // Untouched overlay words may have been written only by the transfer,
        // not by any overlay invocation. Make those visible to CPU readback too.
        {
            barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT|(reuse?VK_ACCESS_TRANSFER_WRITE_BIT:0u);
            barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd_,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|(reuse?VK_PIPELINE_STAGE_TRANSFER_BIT:0u),
                VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        }
        // The resident stream records its own transfer-write -> transfer-read
        // dependency on the same graphics queue before consuming a snapshot.
        if(timestamps_)vkCmdWriteTimestamp(cmd_,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,timestamps_,1);
        check(vkEndCommandBuffer(cmd_),"overlay end");check(vkResetFences(d,1,&fence_),"overlay reset fence");
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&cmd_;
        started_=std::chrono::steady_clock::now();check(vkQueueSubmit(runtime_.graphicsQueue(),1,&si,fence_),"overlay submit");
        if(!treeSelectionReported_ && !work.tree_selections.empty()){treeReportOffset_=selectionOffset;treeReportCount_=uint32_t(work.tree_selections.size());}
        submitted_=true;snapshotOnly_=snapshotOnly;overlayCount_=push[0];overlaySky_=sky;overlaySeeds_=seeds;overlayPropagated_=work.propagate;return true;
    }catch(const std::exception& e){LOG_WARN(std::string("Vulkan overlay CPU fallback: ")+e.what());overlayDisabled_=true;pendingOverlaySnapshot_.reset();return false;}
}
bool VulkanMaterialWorldgen::poll(std::vector<uint32_t>& output,double& ms){
    if(!submitted_||disabled_||snapshotOnly_||certifying_)return false;
    auto r=vkGetFenceStatus(runtime_.device(),fence_);if(r==VK_NOT_READY)return false;if(r!=VK_SUCCESS){disabled_=true;return false;}submitted_=false;
    try{
        if(overlayCount_){
            constexpr size_t B=Gpu_Overlay_Work::block_words,M=Gpu_Overlay_Work::meta_words;
            const size_t lightWords=overlaySky_?Generation_Sky::columns+(overlaySeeds_?Generation_Sky::seed_words:0):0;
            output.resize(B+M+overlayCount_+lightWords+(overlayPropagated_?Generation_Sky::seed_words:0));
            overlayBuffers_[0].read(0,output.data(),B*4);
            overlayBuffers_[1].read(0,output.data()+B,M*4);
            overlayBuffers_[4].read(0,output.data()+B+M,overlayCount_*4);
            if(lightWords) overlayBuffers_[4].read(Generation_Overlay_Packet::group_count*4,
                output.data()+B+M+overlayCount_,lightWords*4);
            if(overlayPropagated_) overlayBuffers_[4].read(
                (Generation_Overlay_Packet::group_count+Generation_Sky::columns+2*Generation_Sky::seed_words)*4,
                output.data()+B+M+overlayCount_+lightWords,Generation_Sky::seed_words*4);
            if(pendingOverlaySnapshot_){
                pendingOverlaySnapshot_->skyRoof=SkyRoofPalette::fromBlocks(std::span<const uint32_t>(output.data(),B));
                pendingOverlaySnapshot_->ready=true;
                lastOverlayReference_={pendingOverlaySnapshot_,pendingOverlaySnapshot_->ticket};
                pendingOverlaySnapshot_.reset();
            }
            overlayCount_=0;overlaySky_=false;overlaySeeds_=false; overlayPropagated_=false;
        }else{output.resize(count_*(voxels/2));buffers_[6].read(0,output.data(),output.size()*sizeof(uint32_t));baseTicket_=baseMode_?nextBaseTicket():0;}
        ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started_).count();
        uint64_t ticks[2]{};if(timestamps_&&vkGetQueryPoolResults(runtime_.device(),timestamps_,0,2,sizeof(ticks),ticks,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT)==VK_SUCCESS){uint64_t mask=timestampBits_==64?UINT64_MAX:((uint64_t(1)<<timestampBits_)-1);ms=double((ticks[1]-ticks[0])&mask)*timestampNs_/1e6;}
        return true;
    }catch(...){disabled_=true;baseTicket_=0;output.clear();return false;}
}
VulkanMaterialWorldgen::SnapshotPoll VulkanMaterialWorldgen::poll_overlay_snapshot(
    std::shared_ptr<OverlaySnapshot>& result) {
    if(!snapshotOnly_)return SnapshotPoll::Idle;
    if(disabled_)return SnapshotPoll::Failed;
    if(!submitted_)return SnapshotPoll::Failed;
    const auto status=vkGetFenceStatus(runtime_.device(),fence_);
    if(status==VK_NOT_READY)return SnapshotPoll::Pending;
    if(status!=VK_SUCCESS){disabled_=true;return SnapshotPoll::Failed;}
    submitted_=false;snapshotOnly_=false;
    overlayCount_=0;overlaySky_=false;overlaySeeds_=false;overlayPropagated_=false;
    if(!pendingOverlaySnapshot_)return SnapshotPoll::Failed;
    if(treeReportCount_){
        // First nonempty batch only, after fence. Diagnostics cannot change publication.
        try {
            std::array<Gpu_Tree_Overlay::Selection,Gpu_Tree_Overlay::max_instances> results{};
            overlayBuffers_[3].read(treeReportOffset_*4ull,results.data(),treeReportCount_*sizeof(results[0]));
            uint32_t matched=0,native=0,rejected=0;for(uint32_t i=0;i<treeReportCount_;++i){matched+=uint32_t((results[i].result&0xffff0000u)==0x80000000u);native+=uint32_t((results[i].result&0xffff0000u)==0x40000000u);rejected+=uint32_t(results[i].result==0x4000ffffu);}
            LOG_INFO("Vulkan tree first batch parity: "+std::to_string(matched)+"/"+std::to_string(treeReportCount_)+"; native decisions: "+std::to_string(native)+" (rejected: "+std::to_string(rejected)+"); unmatched legacy handles retain CPU template");
        }catch(const std::exception&){}
        treeSelectionReported_=true;treeReportCount_=0;
    }
    try {
        std::array<uint32_t,SkyRoofPalette::resultWords> roof{};
        overlayBuffers_[4].read(SkyRoofPalette::resultOffset*4ull,roof.data(),sizeof(roof));
        pendingOverlaySnapshot_->skyRoof=SkyRoofPalette::fromGpu(roof);
    } catch(...) {pendingOverlaySnapshot_->skyRoof={};} // Optional certificate only.
    pendingOverlaySnapshot_->ready=true;
    // Only the compact roof palette is read; no block or metadata readback.
    // Return a strong lease; a weak reference alone could expire before submit.
    result=std::move(pendingOverlaySnapshot_);
    return SnapshotPoll::Ready;
}
VulkanMaterialWorldgen::SnapshotPoll VulkanMaterialWorldgen::poll_base_certificate(
    std::vector<Gpu_Certified_Base>& result) {
    if(!certifying_)return SnapshotPoll::Idle;
    if(disabled_||!submitted_)return SnapshotPoll::Failed;
    const auto status=vkGetFenceStatus(runtime_.device(),fence_);
    if(status==VK_NOT_READY)return SnapshotPoll::Pending;
    if(status!=VK_SUCCESS){disabled_=true;certifiedLevels_={};return SnapshotPoll::Failed;}
    submitted_=false;certifying_=false;
    try {
        std::array<uint32_t,9> counts{};
        if(nativeBase_){
            const auto nativeCounts=nativeColumn_.certificates();
            if(nativeCounts.size()!=count_)throw std::runtime_error("native column certificate count");
            std::copy(nativeCounts.begin(),nativeCounts.end(),counts.begin());
            if(const auto matches=nativeColumn_.caveAccepted();matches&&!caveReported_){
                LOG_INFO("Vulkan cave first column matching mask words: "+std::to_string(*matches)+"/"+std::to_string(count_*2048)+"; unmatched words retain CPU input");caveReported_=true;
            }
            if(const auto matches=nativeColumn_.riverAccepted();matches&&!riverReported_){
                LOG_INFO("Vulkan river first column parity: "+std::to_string(*matches)+"/1024; unmatched columns retain CPU input");riverReported_=true;
            }
            if(const auto matches=nativeColumn_.climateAccepted();matches&&!climateReported_){
                LOG_INFO("Vulkan climate first column parity: "+std::to_string(*matches)+"/1024; unmatched columns retain CPU input");
                climateReported_=true;
            }
        }else buffers_[6].read(count_*(voxels/2)*4,counts.data(),count_*4);
        std::vector<Gpu_Certified_Base> accepted;
        accepted.reserve(count_);
        const auto ticket=nextBaseTicket();
        for(size_t i=0;i<count_;++i){
            if(ticket && counts[i]==voxels/64){certifiedLevels_[i].ticket=ticket;accepted.push_back(certifiedLevels_[i]);}
            else certifiedLevels_[i]={};
        }
        result=std::move(accepted);
        return SnapshotPoll::Ready;
    } catch(...) {certifiedLevels_={};disabled_=true;return SnapshotPoll::Failed;}
}
void VulkanMaterialWorldgen::abandon() noexcept {disabled_=true;count_=0;baseTicket_=0;lastOverlayReference_={};certifiedLevels_={};}
}
