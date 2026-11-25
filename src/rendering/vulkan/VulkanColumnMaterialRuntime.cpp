#include "rendering/vulkan/VulkanColumnMaterialRuntime.h"
#include <fstream>
#include <stdexcept>
#include <string>

namespace OptiCraft::Vulkan {
namespace {
void check(VkResult r,const char* msg){if(r!=VK_SUCCESS)throw std::runtime_error(std::string(msg)+": "+std::to_string(r));}
using P=GpuColumnMaterialPlan;
constexpr size_t index(P::Resource r){return size_t(r);}
struct Spec {const char* name;uint32_t first,count,push;};
constexpr std::array<Spec,4> specs{{
    {"worldgen_material_input.comp",0,3,sizeof(P::Input::Push)},
    {"worldgen_river_pack.comp",6,2,sizeof(Gpu_River_Dispatch::BridgePush)},
    {"worldgen_cave_batch_column_pack.comp",6,2,sizeof(P::Input::CavePush)},
    {"worldgen_material.comp",0,7,sizeof(P::BasePush)}}};
std::vector<uint32_t> load(const std::filesystem::path& path){
    std::ifstream f(path,std::ios::binary|std::ios::ate);const auto bytes=f.tellg();
    if(!f||bytes<20||bytes%4)throw std::runtime_error("Invalid SPIR-V: "+path.string());
    std::vector<uint32_t> v(size_t(bytes)/4);f.seekg(0);
    if(!f.read(reinterpret_cast<char*>(v.data()),std::streamsize(bytes))||v[0]!=0x07230203u)throw std::runtime_error("Invalid SPIR-V: "+path.string());
    return v;
}
void barrier(VkCommandBuffer cmd,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkAccessFlags a,VkAccessFlags b){
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};mb.srcAccessMask=a;mb.dstAccessMask=b;
    vkCmdPipelineBarrier(cmd,src,dst,0,1,&mb,0,nullptr,0,nullptr);
}
}
void VulkanColumnMaterialRuntime::initialize(VkPhysicalDevice physical,VkDevice device,bool fp64,const std::filesystem::path& dir){
    if(device_||!device||!physical||!fp64)throw std::invalid_argument("Column runtime device/FP64");
    VkPhysicalDeviceProperties p{};vkGetPhysicalDeviceProperties(physical,&p);const auto& l=p.limits;
    limits_={fp64,l.maxComputeWorkGroupCount[0],l.maxComputeWorkGroupCount[1],l.maxComputeWorkGroupCount[2],l.maxComputeWorkGroupSize[0],l.maxComputeWorkGroupInvocations,l.maxPerStageDescriptorStorageBuffers,l.maxDescriptorSetStorageBuffers,l.maxPushConstantsSize,l.maxStorageBufferRange,l.minStorageBufferOffsetAlignment};
    if(limits_.perStageStorage<7||limits_.setStorage<7||limits_.pushBytes<64||limits_.localX<64||limits_.invocations<64||!limits_.offsetAlignment)throw std::runtime_error("Column runtime device limits");
    physical_=physical;device_=device;
    try {
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,14};VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=uint32_t(passes_.size());pi.poolSizeCount=1;pi.pPoolSizes=&ps;
        check(vkCreateDescriptorPool(device_,&pi,nullptr,&pool_),"Column descriptor pool");
        for(size_t i=0;i<passes_.size();++i){const auto& s=specs[i];makePass(passes_[i],pool_,dir,s.name,s.first,s.count,s.push);}
    }catch(...){shutdown();throw;}
}
void VulkanColumnMaterialRuntime::makePass(PassGpu& p,VkDescriptorPool pool,const std::filesystem::path& dir,const char* name,uint32_t first,uint32_t count,uint32_t push){
    std::array<VkDescriptorSetLayoutBinding,7> b{};for(uint32_t j=0;j<count;++j)b[j]={first+j,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};dl.bindingCount=count;dl.pBindings=b.data();check(vkCreateDescriptorSetLayout(device_,&dl,nullptr,&p.layout),"Column descriptor layout");
    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};da.descriptorPool=pool;da.descriptorSetCount=1;da.pSetLayouts=&p.layout;check(vkAllocateDescriptorSets(device_,&da,&p.set),"Column descriptor set");
    VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT,0,push};VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&p.layout;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&pc;check(vkCreatePipelineLayout(device_,&pl,nullptr,&p.pipelineLayout),"Column pipeline layout");
    const auto spv=load(dir/(std::string(name)+".spv"));VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=spv.size()*4;sm.pCode=spv.data();VkShaderModule module=VK_NULL_HANDLE;check(vkCreateShaderModule(device_,&sm,nullptr,&module),"Column shader");
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.layout=p.pipelineLayout;ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=module;ci.stage.pName="main";
    const auto status=vkCreateComputePipelines(device_,VK_NULL_HANDLE,1,&ci,nullptr,&p.pipeline);vkDestroyShaderModule(device_,module,nullptr);check(status,"Column pipeline");
}
void VulkanColumnMaterialRuntime::bindPass(const PassGpu& pass,std::span<const Source> sources,uint32_t first){
    std::array<VkDescriptorBufferInfo,7> info{};std::array<VkWriteDescriptorSet,7> writes{};
    for(uint32_t j=0;j<sources.size();++j){const auto& s=sources[j];info[j]={s.buffer,s.offset,s.bytes};auto& w=writes[j];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=pass.set;w.dstBinding=first+j;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w.pBufferInfo=&info[j];}
    vkUpdateDescriptorSets(device_,uint32_t(sources.size()),writes.data(),0,nullptr);
}
void VulkanColumnMaterialRuntime::initializeClimate(const Gpu_Climate_Tables& tables,const std::filesystem::path& dir){
    if(!device_||busy_||climatePool_)throw std::logic_error("Climate runtime lifetime");
    const std::array<uint64_t,3> sizes{sizeof(tables.permutation),sizeof(tables.gradient),sizeof(tables.lattice)};
    for(auto n:sizes)if(n>limits_.storageRange)throw std::runtime_error("Climate table range");
    if(limits_.storageRange<Gpu_Climate_Dispatch::samples*sizeof(Gpu_Climate_Dispatch::Raw)||limits_.groupsX<Gpu_Climate_Dispatch::raw_groups)throw std::runtime_error("Climate dispatch limits");
    try{
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,14};VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=4;pi.poolSizeCount=1;pi.pPoolSizes=&ps;
        check(vkCreateDescriptorPool(device_,&pi,nullptr,&climatePool_),"Climate descriptor pool");
        makePass(climatePasses_[0],climatePool_,dir,"worldgen_climate.comp",0,4,sizeof(Gpu_Climate_Dispatch::RawPush));
        makePass(climatePasses_[1],climatePool_,dir,"worldgen_terrain.comp",0,2,sizeof(Gpu_Climate_Dispatch::TerrainPush));
        makePass(climatePasses_[2],climatePool_,dir,"worldgen_layers.comp",0,2,sizeof(Gpu_Layer_Dispatch::Push));
        makePass(climatePasses_[3],climatePool_,dir,"worldgen_layer_merge.comp",0,6,4);
        const std::array<const void*,3> data{tables.permutation.data(),tables.gradient.data(),tables.lattice.data()};
        for(size_t i=0;i<sizes.size();++i){ensure(climateTables_[i],sizes[i],VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);climateTables_[i].write(0,data[i],sizes[i]);}
    }catch(...){destroyClimate();throw;}
}
void VulkanColumnMaterialRuntime::destroyClimate() noexcept {
    for(auto& p:climatePasses_){if(p.pipeline)vkDestroyPipeline(device_,p.pipeline,nullptr);if(p.pipelineLayout)vkDestroyPipelineLayout(device_,p.pipelineLayout,nullptr);}
    if(climatePool_)vkDestroyDescriptorPool(device_,climatePool_,nullptr);
    for(auto& p:climatePasses_){if(p.layout)vkDestroyDescriptorSetLayout(device_,p.layout,nullptr);p={};}
    climatePool_=VK_NULL_HANDLE;for(auto& b:climateTables_)b.destroy();rawClimate_.destroy();terrain_.destroy();layers_.destroy();climateStats_.destroy();
}
bool VulkanColumnMaterialRuntime::recordClimatePrepared(VkCommandBuffer cmd,int32_t x,int32_t z,std::span<const int32_t> levels,
                    int32_t minY,int32_t maxY,std::span<const uint32_t,10> oreIds,uint64_t oreNodes,const Sources& ext,const Gpu_Layer_Dispatch::Push& layer,const Gpu_Cave_Dispatch::Packet* cave){
    if(!device_||!cmd||busy_||!climatePool_)return false;
    Gpu_Climate_Dispatch::RawPush origin{};
    if(!Gpu_Climate_Dispatch::make(x,z,origin)||layer.origin_x!=origin.origin_x||layer.origin_z!=origin.origin_z||layer.single_biome>1||layer.selected_biome>=39)return false;
    Plan p;
    if(!Plan::makePrepared(x,z,levels,minY,maxY,oreIds,oreNodes,limits_,p))return false;
    return recordPlan(cmd,p,ext,true,&layer,nullptr,cave);
}
void VulkanColumnMaterialRuntime::initializeRiver(const std::filesystem::path& dir){
    using D=Gpu_River_Dispatch;
    if(!device_||busy_||riverPool_)throw std::logic_error("River runtime lifetime");
    if(limits_.storageRange<D::compact_cells*4ull||limits_.groupsX<D::column_resolve_groups)throw std::runtime_error("River dispatch limits");
    try {
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,7};VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=2;pi.poolSizeCount=1;pi.pPoolSizes=&ps;
        check(vkCreateDescriptorPool(device_,&pi,nullptr,&riverPool_),"River descriptor pool");
        makePass(riverPasses_[0],riverPool_,dir,"worldgen_river_column_resolve.comp",0,3,sizeof(D::ColumnPush));
        makePass(riverPasses_[1],riverPool_,dir,"worldgen_river_column_merge.comp",0,4,4);
    }catch(...){destroyRiver();throw;}
}
void VulkanColumnMaterialRuntime::destroyRiver() noexcept {
    for(auto& p:riverPasses_){if(p.pipeline)vkDestroyPipeline(device_,p.pipeline,nullptr);if(p.pipelineLayout)vkDestroyPipelineLayout(device_,p.pipelineLayout,nullptr);}
    if(riverPool_)vkDestroyDescriptorPool(device_,riverPool_,nullptr);
    for(auto& p:riverPasses_){if(p.layout)vkDestroyDescriptorSetLayout(device_,p.layout,nullptr);p={};}
    riverPool_=VK_NULL_HANDLE;riverCached_.reset();riverCoarse_.destroy();riverGraph_.destroy();riverResolved_.destroy();riverStats_.destroy();
}
bool VulkanColumnMaterialRuntime::recordClimateRiverPrepared(VkCommandBuffer cmd,int32_t x,int32_t z,std::span<const int32_t> levels,
                    int32_t minY,int32_t maxY,std::span<const uint32_t,10> oreIds,uint64_t oreNodes,const Sources& ext,const Gpu_Layer_Dispatch::Push& layer,
                    std::shared_ptr<const Gpu_River_Dispatch::CompactPacket> packet,const Gpu_Cave_Dispatch::Packet* cave){
    using D=Gpu_River_Dispatch;
    if(!device_||!cmd||busy_||!climatePool_||!riverPool_||!packet)return false;
    Gpu_Climate_Dispatch::RawPush origin{};D::BridgePush offset{};
    if(!Gpu_Climate_Dispatch::make(x,z,origin)||layer.origin_x!=origin.origin_x||layer.origin_z!=origin.origin_z||layer.single_biome>1||layer.selected_biome>=39||
       !D::bridge(packet->region_x,packet->region_z,x,z,offset))return false;
    Plan p;if(!Plan::makePrepared(x,z,levels,minY,maxY,oreIds,oreNodes,limits_,p))return false;
    if(riverCached_!=packet){
        if(!packet->valid())return false;
        riverCached_.reset(); // a failed second upload must not retain a stale cache identity
        constexpr auto host=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,coherent=VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        ensure(riverCoarse_,D::compact_coarse_cells*4ull,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,host,coherent);
        ensure(riverGraph_,D::compact_cells*4ull,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,host,coherent);
        riverCoarse_.write(0,packet->coarse.data(),D::compact_coarse_cells*4ull);
        riverGraph_.write(0,packet->graph.data(),D::compact_cells*4ull);
        riverCached_=std::move(packet);
    }
    const D::ColumnPush push{riverCached_->sea,offset.offset_x,offset.offset_z};
    return recordPlan(cmd,p,ext,true,&layer,&push,cave);
}
void VulkanColumnMaterialRuntime::initializeCave(const std::filesystem::path& dir){
    using D=Gpu_Cave_Dispatch;
    if(!device_||busy_||cavePool_)throw std::logic_error("Cave runtime lifetime");
    if(limits_.storageRange<D::kind_words*4ull||limits_.groupsX<D::resolve_groups)throw std::runtime_error("Cave dispatch limits");
    try{
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,13};VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=3;pi.poolSizeCount=1;pi.pPoolSizes=&ps;
        check(vkCreateDescriptorPool(device_,&pi,nullptr,&cavePool_),"Cave descriptor pool");
        makePass(cavePasses_[0],cavePool_,dir,"worldgen_cave_carve.comp",0,6,sizeof(D::Push));
        makePass(cavePasses_[1],cavePool_,dir,"worldgen_cave_resolve.comp",4,3,sizeof(D::Push));
        makePass(cavePasses_[2],cavePool_,dir,"worldgen_cave_column_merge.comp",0,4,4);
    }catch(...){destroyCave();throw;}
}
bool VulkanColumnMaterialRuntime::prepareCave(const Plan& p,const Gpu_Cave_Dispatch::Packet& packet){
    using D=Gpu_Cave_Dispatch;
    if(!cavePool_||!packet.valid_for(p.base.x,p.base.z)||packet.push.count>limits_.groupsX||p.init.count>limits_.groupsY)return false;
    const auto stampBytes=std::max<size_t>(1,packet.stamps.size())*sizeof(D::Stamp);
    constexpr auto storage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    auto upload=[&](VulkanDeviceBuffer& b,const void* data,VkDeviceSize n){
        if(n>limits_.storageRange)throw std::runtime_error("Cave source range");
        ensure(b,n,storage,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);b.write(0,data,n);
    };
    upload(caveTables_[0],packet.tables.permutation.data(),sizeof(packet.tables.permutation));
    upload(caveTables_[1],packet.tables.gradient.data(),sizeof(packet.tables.gradient));
    upload(caveTables_[2],packet.tables.lattice.data(),sizeof(packet.tables.lattice));
    D::Stamp empty{};upload(caveStamps_,packet.stamps.empty()?&empty:packet.stamps.data(),stampBytes);
    upload(caveHeights_,packet.heights.data(),sizeof(packet.heights));
    ensure(caveMasks_,D::mask_words*8ull,storage|VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    ensure(caveKinds_,D::kind_words*4ull,storage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    ensure(caveStats_,4,storage|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    return true;
}
void VulkanColumnMaterialRuntime::destroyCave() noexcept {
    for(auto& p:cavePasses_){if(p.pipeline)vkDestroyPipeline(device_,p.pipeline,nullptr);if(p.pipelineLayout)vkDestroyPipelineLayout(device_,p.pipelineLayout,nullptr);}
    if(cavePool_)vkDestroyDescriptorPool(device_,cavePool_,nullptr);
    for(auto& p:cavePasses_){if(p.layout)vkDestroyDescriptorSetLayout(device_,p.layout,nullptr);p={};}
    cavePool_=VK_NULL_HANDLE;for(auto& b:caveTables_)b.destroy();caveStamps_.destroy();caveHeights_.destroy();caveMasks_.destroy();caveKinds_.destroy();caveStats_.destroy();
}
std::optional<uint32_t> VulkanColumnMaterialRuntime::caveAccepted() const {
    if(!busy_||!caveUsed_)return std::nullopt;
    uint32_t accepted=0;readback_.read(uint64_t(certCount_)*4+4+(riverUsed_?4:0),&accepted,4);
    if(accepted>certCount_*2048)throw std::runtime_error("Invalid cave match count");
    return accepted;
}
void VulkanColumnMaterialRuntime::ensure(VulkanDeviceBuffer& buffer,VkDeviceSize bytes,VkBufferUsageFlags usage,VkMemoryPropertyFlags required,VkMemoryPropertyFlags preferred){
    if(buffer.size()>=bytes)return;
    VulkanDeviceBuffer next;next.create(physical_,device_,bytes,usage,required,preferred);buffer=std::move(next);
}
bool VulkanColumnMaterialRuntime::record(VkCommandBuffer cmd,const Request& r,const Sources& ext){
    if(!device_||!cmd||busy_)return false;
    Plan p;
    if(!Plan::make(r.x,r.z,r.levels,r.sea,r.water,r.floor,r.sky,r.minY,r.maxY,r.oreIds,r.oreNodes,r.river,r.cave,limits_,p))return false;
    return recordPlan(cmd,p,ext,false);
}
bool VulkanColumnMaterialRuntime::recordPrepared(VkCommandBuffer cmd,int32_t x,int32_t z,std::span<const int32_t> levels,
                    int32_t minY,int32_t maxY,std::span<const uint32_t,10> oreIds,uint64_t oreNodes,const Sources& ext){
    if(!device_||!cmd||busy_)return false;
    Plan p;
    if(!Plan::makePrepared(x,z,levels,minY,maxY,oreIds,oreNodes,limits_,p))return false;
    return recordPlan(cmd,p,ext,true);
}
bool VulkanColumnMaterialRuntime::recordPlan(VkCommandBuffer cmd,const Plan& p,const Sources& ext,bool prepared,const Gpu_Layer_Dispatch::Push* climate,const Gpu_River_Dispatch::ColumnPush* river,const Gpu_Cave_Dispatch::Packet* cave){
    auto sources=ext;
    for(size_t i=0;i<sources.size();++i){const auto& s=sources[i];const auto res=Resource(i);
        const bool internal=res==Resource::Output||(!prepared&&(res==Resource::Columns||res==Resource::InputWords));
        if(internal||!p.bytes[i]){if(s.buffer||s.bytes||s.capacity||s.offset)return false;continue;}
        if(!s.buffer||!Plan::validSlice({s.offset,s.bytes,s.capacity},p.bytes[i],limits_))return false;
    }
    // Reject stale scratch aliases before growing or destroying any allocation.
    for(size_t i=0;i<ext.size();++i)if(ext[i].buffer&&(ext[i].buffer==columns_.buffer()||ext[i].buffer==words_.buffer()||ext[i].buffer==output_.buffer()||ext[i].buffer==readback_.buffer()))return false;
    // Optional cave data is prepared on a worker. Reject malformed packets or
    // allocation failure without touching the established reference masks.
    if(cave){try{if(!climate||!prepareCave(p,*cave))cave=nullptr;}catch(const std::exception&){cave=nullptr;}}
    // All allocations happen before any command recording. Scratch is never
    // resized while a submission or external result consumer holds it.
    constexpr auto storage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if(!prepared||climate){
        ensure(columns_,p.bytes[index(Resource::Columns)],storage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        ensure(words_,p.bytes[index(Resource::InputWords)],storage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    ensure(output_,p.bytes[index(Resource::Output)],storage|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    ensure(readback_,p.certificateBytes+(climate?4:0)+(river?4:0)+(cave?4:0),VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    auto own=[&](Resource res,const VulkanDeviceBuffer& b){sources[index(res)]={b.buffer(),0,p.bytes[index(res)],b.size()};};
    if(climate){
        const auto rawBytes=Gpu_Climate_Dispatch::samples*sizeof(Gpu_Climate_Dispatch::Raw);
        const auto terrainBytes=Gpu_Climate_Dispatch::columns*sizeof(Gpu_Climate_Dispatch::Terrain);
        const auto layerBytes=Gpu_Layer_Dispatch::columns*sizeof(Gpu_Layer_Dispatch::Layer);
        ensure(rawClimate_,rawBytes,storage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        ensure(terrain_,terrainBytes,storage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        ensure(layers_,layerBytes,storage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        ensure(climateStats_,4,storage|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        auto slice=[](const VulkanDeviceBuffer& b,VkDeviceSize n){return Source{b.buffer(),0,n,b.size()};};
        const auto raw=slice(rawClimate_,rawBytes),terrain=slice(terrain_,terrainBytes),layer=slice(layers_,layerBytes);
        bindPass(climatePasses_[0],std::array{slice(climateTables_[0],climateTables_[0].size()),slice(climateTables_[1],climateTables_[1].size()),slice(climateTables_[2],climateTables_[2].size()),raw});
        bindPass(climatePasses_[1],std::array{raw,terrain});
        bindPass(climatePasses_[2],std::array{terrain,layer});
        bindPass(climatePasses_[3],std::array{layer,ext[index(Resource::Columns)],ext[index(Resource::InputWords)],slice(columns_,p.bytes[index(Resource::Columns)]),slice(words_,p.bytes[index(Resource::InputWords)]),slice(climateStats_,4)});
    }
    if(river){
        using D=Gpu_River_Dispatch;
        ensure(riverResolved_,D::column_resolved_cells*16ull,storage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        ensure(riverStats_,4,storage|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        auto slice=[](const VulkanDeviceBuffer& b,VkDeviceSize n){return Source{b.buffer(),0,n,b.size()};};
        const auto resolved=slice(riverResolved_,D::column_resolved_cells*16ull);
        bindPass(riverPasses_[0],std::array{slice(riverCoarse_,D::compact_coarse_cells*4ull),slice(riverGraph_,D::compact_cells*4ull),resolved});
        bindPass(riverPasses_[1],std::array{resolved,ext[index(Resource::InputWords)],slice(words_,p.bytes[index(Resource::InputWords)]),slice(riverStats_,4)});
    }
    if(cave){
        using D=Gpu_Cave_Dispatch;
        auto slice=[](const VulkanDeviceBuffer& b,VkDeviceSize n){return Source{b.buffer(),0,n,b.size()};};
        const auto heights=slice(caveHeights_,sizeof(cave->heights)),masks=slice(caveMasks_,D::mask_words*8ull),kinds=slice(caveKinds_,D::kind_words*4ull);
        bindPass(cavePasses_[0],std::array{slice(caveTables_[0],sizeof(cave->tables.permutation)),slice(caveTables_[1],sizeof(cave->tables.gradient)),slice(caveTables_[2],sizeof(cave->tables.lattice)),slice(caveStamps_,std::max<size_t>(1,cave->stamps.size())*sizeof(D::Stamp)),heights,masks});
        bindPass(cavePasses_[1],std::array{heights,masks,kinds},4);
        bindPass(cavePasses_[2],std::array{kinds,ext[index(Resource::InputWords)],slice(words_,p.bytes[index(Resource::InputWords)]),slice(caveStats_,4)});
    }
    if(!prepared||climate){own(Resource::Columns,columns_);own(Resource::InputWords,words_);}
    own(Resource::Output,output_);
    for(uint32_t i=0;i<p.stepCount;++i){const auto& step=p.steps[i];const auto& pass=passes_[size_t(step.pass)];
        std::array<VkDescriptorBufferInfo,7> info{};std::array<VkWriteDescriptorSet,7> writes{};
        for(uint32_t j=0;j<step.bindingCount;++j){const auto& s=sources[index(step.bindings[j].resource)];info[j]={s.buffer,s.offset,s.bytes};auto& w=writes[j];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=pass.set;w.dstBinding=step.bindings[j].slot;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w.pBufferInfo=&info[j];}
        vkUpdateDescriptorSets(device_,step.bindingCount,writes.data(),0,nullptr);
    }
    if(cave){vkCmdFillBuffer(cmd,caveMasks_.buffer(),0,Gpu_Cave_Dispatch::mask_words*8ull,0);vkCmdFillBuffer(cmd,caveStats_.buffer(),0,4,0);}
    if(river)vkCmdFillBuffer(cmd,riverStats_.buffer(),0,4,0);
    if(climate)vkCmdFillBuffer(cmd,climateStats_.buffer(),0,4,0);
    vkCmdFillBuffer(cmd,output_.buffer(),p.certificateOffset,p.certificateBytes,0);
    barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
    if(climate){
        const Gpu_Climate_Dispatch::RawPush raw{climate->origin_x,climate->origin_z};
        const Gpu_Climate_Dispatch::TerrainPush terrain{p.base.minY,p.base.maxY};
        const uint32_t count=p.init.count;
        const std::array<const void*,4> push{&raw,&terrain,climate,&count};
        const std::array<uint32_t,4> sizes{sizeof(raw),sizeof(terrain),sizeof(*climate),sizeof(count)};
        const std::array<uint32_t,4> groups{Gpu_Climate_Dispatch::raw_groups,Gpu_Climate_Dispatch::terrain_groups,Gpu_Layer_Dispatch::groups,16};
        for(size_t i=0;i<climatePasses_.size();++i){const auto& pass=climatePasses_[i];
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pass.pipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pass.pipelineLayout,0,1,&pass.set,0,nullptr);vkCmdPushConstants(cmd,pass.pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizes[i],push[i]);vkCmdDispatch(cmd,groups[i],1,1);
            barrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT);
        }
    }
    if(river){
        const auto& r=riverPasses_[0];
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,r.pipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,r.pipelineLayout,0,1,&r.set,0,nullptr);vkCmdPushConstants(cmd,r.pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(*river),river);vkCmdDispatch(cmd,Gpu_River_Dispatch::column_resolve_groups,1,1);
        barrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
        const auto& m=riverPasses_[1];
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,m.pipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,m.pipelineLayout,0,1,&m.set,0,nullptr);vkCmdPushConstants(cmd,m.pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&river->sea);vkCmdDispatch(cmd,Gpu_River_Dispatch::column_merge_groups,1,1);
        barrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT);
    }
    if(cave){
        const uint32_t count=p.init.count;
        const std::array<const void*,3> pushes{&cave->push,&cave->push,&count};
        const std::array<uint32_t,3> sizes{sizeof(cave->push),sizeof(cave->push),4};
        const std::array<uint32_t,3> gx{cave->push.count,Gpu_Cave_Dispatch::resolve_groups,32};
        for(size_t i=0;i<3;++i){if(!gx[i])continue;const auto& pass=cavePasses_[i];
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pass.pipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pass.pipelineLayout,0,1,&pass.set,0,nullptr);vkCmdPushConstants(cmd,pass.pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizes[i],pushes[i]);vkCmdDispatch(cmd,gx[i],i==2?count:1,1);
            barrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT);
        }
    }
    for(uint32_t i=0;i<p.stepCount;++i){const auto& step=p.steps[i];const auto& pass=passes_[size_t(step.pass)];const void* push=nullptr;
        switch(step.pass){case Plan::Pass::Init:push=&p.init;break;case Plan::Pass::RiverPack:push=&p.river;break;case Plan::Pass::CavePack:push=&p.cave;break;case Plan::Pass::Base:push=&p.base;break;}
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pass.pipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pass.pipelineLayout,0,1,&pass.set,0,nullptr);vkCmdPushConstants(cmd,pass.pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,step.pushBytes,push);vkCmdDispatch(cmd,step.x,step.y,step.z);
        barrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT);
    }
    const VkBufferCopy copy{p.certificateOffset,0,p.certificateBytes};vkCmdCopyBuffer(cmd,output_.buffer(),readback_.buffer(),1,&copy);
    if(climate){const VkBufferCopy stats{0,p.certificateBytes,4};vkCmdCopyBuffer(cmd,climateStats_.buffer(),readback_.buffer(),1,&stats);}
    if(river){const VkBufferCopy stats{0,p.certificateBytes+4,4};vkCmdCopyBuffer(cmd,riverStats_.buffer(),readback_.buffer(),1,&stats);}
    if(cave){const VkBufferCopy stats{0,p.certificateBytes+4+(river?4:0),4};vkCmdCopyBuffer(cmd,caveStats_.buffer(),readback_.buffer(),1,&stats);}
    barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);
    climateUsed_=bool(climate);riverUsed_=bool(river);caveUsed_=bool(cave);certCount_=p.init.count;resultBytes_=p.certificateOffset;busy_=true;return true;
}
VulkanColumnMaterialRuntime::Source VulkanColumnMaterialRuntime::result() const noexcept {
    return busy_?Source{output_.buffer(),0,resultBytes_,output_.size()}:Source{};
}
std::vector<uint32_t> VulkanColumnMaterialRuntime::certificates() const {
    if(!busy_)return {};
    std::vector<uint32_t> result(certCount_);readback_.read(0,result.data(),result.size()*sizeof(uint32_t));return result;
}
std::optional<uint32_t> VulkanColumnMaterialRuntime::climateAccepted() const {
    if(!busy_||!climateUsed_)return std::nullopt;
    uint32_t accepted=0;readback_.read(uint64_t(certCount_)*4,&accepted,4);
    if(accepted>1024)throw std::runtime_error("Invalid climate match count");
    return accepted;
}
std::optional<uint32_t> VulkanColumnMaterialRuntime::riverAccepted() const {
    if(!busy_||!riverUsed_)return std::nullopt;
    uint32_t accepted=0;readback_.read(uint64_t(certCount_)*4+4,&accepted,4);
    if(accepted>1024)throw std::runtime_error("Invalid river match count");
    return accepted;
}
void VulkanColumnMaterialRuntime::shutdown() noexcept {
    if(!device_)return;
    destroyCave();destroyRiver();destroyClimate();
    for(auto& p:passes_){if(p.pipeline)vkDestroyPipeline(device_,p.pipeline,nullptr);if(p.pipelineLayout)vkDestroyPipelineLayout(device_,p.pipelineLayout,nullptr);}
    if(pool_)vkDestroyDescriptorPool(device_,pool_,nullptr);
    for(auto& p:passes_){if(p.layout)vkDestroyDescriptorSetLayout(device_,p.layout,nullptr);p={};}
    columns_.destroy();words_.destroy();output_.destroy();readback_.destroy();retire();pool_=VK_NULL_HANDLE;device_=VK_NULL_HANDLE;physical_=VK_NULL_HANDLE;
}
}
