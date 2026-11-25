#include "rendering/vulkan/VulkanResidentLight.h"
#include "rendering/vulkan/VulkanChunkStreamGpu.h"
#include <array>
#include <algorithm>
#include <stdexcept>

namespace OptiCraft::Vulkan {
namespace {
constexpr std::array<uint32_t,6> bindings{0,1,2,3,4,9};
constexpr VkDeviceSize lightBytes=ResidentLightBatch::words*sizeof(uint32_t);
void require(VkResult r,const char* what){if(r!=VK_SUCCESS)throw VulkanBufferError(r,what);}
void dependency(VkCommandBuffer cmd,VkPipelineStageFlags src,VkAccessFlags sa,
                VkPipelineStageFlags dst,VkAccessFlags da){
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};b.srcAccessMask=sa;b.dstAccessMask=da;
    vkCmdPipelineBarrier(cmd,src,dst,0,1,&b,0,nullptr,0,nullptr);
}
}
VulkanResidentLight::~VulkanResidentLight(){destroy();}
void VulkanResidentLight::destroy() noexcept {
    if(device_){
        if(pipeline_)vkDestroyPipeline(device_,pipeline_,nullptr);
        if(pipelineLayout_)vkDestroyPipelineLayout(device_,pipelineLayout_,nullptr);
        if(pool_)vkDestroyDescriptorPool(device_,pool_,nullptr);
        if(layout_)vkDestroyDescriptorSetLayout(device_,layout_,nullptr);
    }
    pipeline_=VK_NULL_HANDLE;pipelineLayout_=VK_NULL_HANDLE;pool_=VK_NULL_HANDLE;
    layout_=VK_NULL_HANDLE;set_=VK_NULL_HANDLE;
    slots_.destroy();jobs_.destroy();transparency_.destroy();scratch_.destroy();
    device_=VK_NULL_HANDLE;maxJobs_=0;maxRange_=0;retire();
}
void VulkanResidentLight::initialize(VkPhysicalDevice physical,VkDevice device,uint32_t maxJobs,
                                     std::span<const uint32_t> spirv){
    if(device_||!physical||!device||!maxJobs||maxJobs>65535||spirv.empty()||spirv.front()!=0x07230203u)
        throw std::invalid_argument("Resident light: invalid initialization");
    VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(physical,&props);
    const auto& l=props.limits;
    const auto tableWords=ResidentLightBatch::transparencyWords+std::max(ResidentLightBatch::emissionWords,maxJobs*32u);
    if(l.maxComputeWorkGroupCount[0]<128||l.maxComputeWorkGroupCount[1]<maxJobs||
       l.maxComputeWorkGroupSize[0]<64||l.maxComputeWorkGroupInvocations<64||
       l.maxPushConstantsSize<sizeof(ResidentLightBatch::Params)||
       l.maxPerStageDescriptorStorageBuffers<bindings.size()||l.maxDescriptorSetStorageBuffers<bindings.size()||
       2*lightBytes*maxJobs>l.maxStorageBufferRange||
       tableWords*sizeof(uint32_t)>l.maxStorageBufferRange)
        throw std::invalid_argument("Resident light: batch exceeds device limits");
    device_=device;maxJobs_=maxJobs;maxRange_=l.maxStorageBufferRange;
    try{
        const auto host=[&](VulkanDeviceBuffer& b,VkDeviceSize bytes){
            b.create(physical,device,bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        };
        host(slots_,VkDeviceSize(maxJobs)*sizeof(GpuChunkSlot));
        host(jobs_,VkDeviceSize(maxJobs)*sizeof(ResidentLightBatch::Job));
        host(transparency_,tableWords*sizeof(uint32_t));
        scratch_.create(physical,device,2*lightBytes*maxJobs,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        std::array<VkDescriptorSetLayoutBinding,bindings.size()> bs{};
        for(size_t i=0;i<bs.size();++i){bs[i].binding=bindings[i];bs[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bs[i].descriptorCount=1;bs[i].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;}
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dl.bindingCount=uint32_t(bs.size());dl.pBindings=bs.data();
        require(vkCreateDescriptorSetLayout(device_,&dl,nullptr,&layout_),"resident light descriptor layout");
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,uint32_t(bs.size())};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.maxSets=1;dp.poolSizeCount=1;dp.pPoolSizes=&ps;
        require(vkCreateDescriptorPool(device_,&dp,nullptr,&pool_),"resident light descriptor pool");
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        da.descriptorPool=pool_;da.descriptorSetCount=1;da.pSetLayouts=&layout_;
        require(vkAllocateDescriptorSets(device_,&da,&set_),"resident light descriptor set");
        VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(ResidentLightBatch::Params)};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount=1;pl.pSetLayouts=&layout_;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&pc;
        require(vkCreatePipelineLayout(device_,&pl,nullptr,&pipelineLayout_),"resident light pipeline layout");
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        sm.codeSize=spirv.size_bytes();sm.pCode=spirv.data();VkShaderModule shader=VK_NULL_HANDLE;
        require(vkCreateShaderModule(device_,&sm,nullptr,&shader),"resident light shader module");
        VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cp.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};cp.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;
        cp.stage.module=shader;cp.stage.pName="main";cp.layout=pipelineLayout_;
        const auto r=vkCreateComputePipelines(device_,VK_NULL_HANDLE,1,&cp,nullptr,&pipeline_);
        vkDestroyShaderModule(device_,shader,nullptr);require(r,"resident light compute pipeline");
    }catch(...){destroy();throw;}
}
bool VulkanResidentLight::record(VkCommandBuffer cmd,const Sources& s,
                                 std::span<const GpuChunkSlot> slots,std::span<const uint32_t> selected,
                                 std::span<const uint32_t> bits){
    return recordImpl(cmd,s,slots,selected,bits,{});
}
bool VulkanResidentLight::recordGenerated(VkCommandBuffer cmd,const Sources& s,
                                 std::span<const GpuChunkSlot> slots,std::span<const uint32_t> selected,
                                 std::span<const uint32_t> bits,std::span<const uint8_t> emission){
    if(emission.size()!=65536)return false;
    return recordImpl(cmd,s,slots,selected,bits,emission);
}
bool VulkanResidentLight::recordSky(VkCommandBuffer cmd,const Sources& s,
                                 std::span<const GpuChunkSlot> slots,std::span<const uint32_t> selected,
                                 std::span<const uint32_t> bits,std::span<const uint32_t> topOpen){
    if(selected.empty()||selected.size()>maxJobs_||topOpen.size()!=selected.size()*32)return false;
    return recordImpl(cmd,s,slots,selected,bits,{},topOpen);
}
bool VulkanResidentLight::recordSkyCore(VkCommandBuffer cmd,const Sources& s,
                                 std::span<const GpuChunkSlot> slots,std::span<const uint32_t> selected,
                                 uint32_t coreCount,std::span<const uint32_t> bits,
                                 std::span<const uint32_t> topOpen,LightVerticalBounds bounds){
    if(!device_||busy_||!coreCount||coreCount>selected.size()||selected.size()>maxJobs_)return false;
    std::vector<uint32_t> halo;
    if(!ResidentLightBatch::selectCore(slots,selected.first(coreCount),maxJobs_,halo,bounds))return false;
    for(auto id:selected)if(id>=slots.size() || !bounds.contains(int64_t(slots[id].origin[1])/32))return false;
    std::vector<uint32_t> sorted(selected.begin(),selected.end());std::sort(sorted.begin(),sorted.end());
    for(auto id:halo)if(!std::binary_search(sorted.begin(),sorted.end(),id))return false;
    if(!recordSky(cmd,s,slots,selected,bits,topOpen))return false;
    coreCount_=coreCount;return true;
}
bool VulkanResidentLight::recordGeneratedCore(VkCommandBuffer cmd,const Sources& s,
                                 std::span<const GpuChunkSlot> slots,std::span<const uint32_t> core,
                                 std::span<const uint32_t> bits,std::span<const uint8_t> emission,LightVerticalBounds bounds){
    if(!device_||busy_||emission.size()!=65536)return false;
    std::vector<uint32_t> selected;
    if(!ResidentLightBatch::selectCore(slots,core,maxJobs_,selected,bounds))return false;
    if(!recordGenerated(cmd,s,slots,selected,bits,emission))return false;
    coreCount_=uint32_t(core.size());return true;
}
bool VulkanResidentLight::recordStreamCore(VkCommandBuffer cmd,uint32_t frameSlot,const VulkanChunkStreamGpu& stream,
        std::span<const ChunkHandle> core,std::span<const uint32_t> bits,std::span<const uint8_t> emission){
    if(!device_||device_!=stream.device_||busy_||emission.size()!=65536||bits.size()!=2048)return false;
    auto snapshot=stream.lightFrameSnapshot(cmd,frameSlot,core,maxJobs_);
    if(!snapshot)return false;
    const auto sizes=stream.voxelSizes();
    Sources sources{stream.blocks(),stream.lights(),sizes[0],sizes[2],snapshot->serial};
    std::vector<ChunkHandle> handles(core.begin(),core.end());
    std::vector<uint64_t> serials;serials.reserve(core.size());
    for(auto h:core)serials.push_back(stream.dataSerials_[h.slot]);
    if(!recordGeneratedCore(cmd,sources,snapshot->slots,snapshot->core,bits,emission,worldLightBounds))return false;
    streamCore_=std::move(handles);streamDataSerials_=std::move(serials);stream_=&stream;return true;
}
bool VulkanResidentLight::recordStreamSkyCore(VkCommandBuffer cmd,uint32_t frameSlot,const VulkanChunkStreamGpu& stream,
        std::span<const ChunkHandle> core,std::span<const uint32_t> bits,SkyBoundary boundary){
    if(!device_||device_!=stream.device_||busy_||bits.size()!=2048)return false;
    auto snapshot=stream.lightFrameSnapshot(cmd,frameSlot,core,maxJobs_,
        boundary.hasSky?std::optional<int32_t>{boundary.highestChunkY}:std::nullopt,!boundary.hasSky,false,bits);
    if(!snapshot)return false;
    std::vector<uint32_t> masks(snapshot->selected.size()*32,0);
    if(boundary.hasSky)for(size_t i=0;i<snapshot->selected.size();++i){
        const auto id=snapshot->selected[i];
        const auto& slot=snapshot->slots[id];
        const auto r=stream.plan().slotInfo(id);
        if(int64_t(slot.origin[1])==int64_t(boundary.highestChunkY)*32 &&
           r && !stream.skyOccluder(r->key,bits))
            std::fill_n(masks.begin()+i*32,32,UINT32_MAX);
    }
    const auto sizes=stream.voxelSizes();
    Sources sources{stream.blocks(),stream.lights(),sizes[0],sizes[2],snapshot->serial};
    std::vector<ChunkHandle> handles(core.begin(),core.end());
    std::vector<uint64_t> serials;serials.reserve(core.size());
    for(auto h:core)serials.push_back(stream.dataSerials_[h.slot]);
    if(boundary.hasSky){
        if(!recordSkyCore(cmd,sources,snapshot->slots,snapshot->selected,uint32_t(core.size()),bits,masks,
                {worldLightBounds.low,boundary.highestChunkY}))return false;
    }else{
        // No solar source anywhere in this dimension: zero sky is exact
        // even with missing neighbours. Preserve block light via seed pass.
        if(!recordSky(cmd,sources,snapshot->slots,snapshot->selected,bits,masks))return false;
        coreCount_=uint32_t(core.size());
    }
    streamCore_=std::move(handles);streamDataSerials_=std::move(serials);streamSky_=true;stream_=&stream;return true;
}
bool VulkanResidentLight::recordStreamPreviewBlock(VkCommandBuffer cmd,uint32_t frameSlot,const VulkanChunkStreamGpu& stream,
        ChunkHandle core,std::span<const uint32_t> bits,std::span<const uint8_t> emission,bool includeResidentHalo){
    if(!device_||device_!=stream.device_||busy_||bits.size()!=2048||emission.size()!=65536)return false;
    const bool halo=includeResidentHalo&&maxJobs_>=27;
    auto snapshot=stream.lightFrameSnapshot(cmd,frameSlot,std::span(&core,1),maxJobs_,std::nullopt,!halo,halo);
    if(!snapshot)return false;
    const auto sizes=stream.voxelSizes();
    Sources sources{stream.blocks(),stream.lights(),sizes[0],sizes[2],snapshot->serial};
    if(!recordGenerated(cmd,sources,snapshot->slots,snapshot->selected,bits,emission))return false;
    coreCount_=1;streamCore_={core};streamDataSerials_={stream.dataSerials_[core.slot]};
    stream_=&stream;return true;
}
bool VulkanResidentLight::recordStreamPreviewSky(VkCommandBuffer cmd,uint32_t frameSlot,const VulkanChunkStreamGpu& stream,
        ChunkHandle core,std::span<const uint32_t> bits,std::span<const uint32_t> topOpen,
        std::span<const PreviewSkySeed> haloSeeds,bool includeResidentHalo,std::optional<int32_t> previewCeiling){
    if(!device_||device_!=stream.device_||busy_||bits.size()!=2048||topOpen.size()!=32)return false;
    const bool halo=includeResidentHalo&&maxJobs_>=27;
    auto snapshot=stream.lightFrameSnapshot(cmd,frameSlot,std::span(&core,1),maxJobs_,halo?previewCeiling:std::nullopt,!halo,halo,bits);
    if(!snapshot)return false;
    const auto sizes=stream.voxelSizes();
    Sources sources{stream.blocks(),stream.lights(),sizes[0],sizes[2],snapshot->serial};
    std::vector<uint32_t> masks(snapshot->selected.size()*32,0);
    for(size_t i=0;i<snapshot->selected.size();++i){
        const auto id=snapshot->selected[i];
        const auto upper=snapshot->slots[id].neighborA[3];
        if(upper>=0 && size_t(upper)<snapshot->slots.size() && snapshot->slots[upper].source[3])continue;
        const auto r=stream.plan().slotInfo(id);if(!r)continue;
        // No direct seed can survive a certified opaque highest plane.
        // Avoid unnecessary lateral dispatches even at the physical ceiling.
        if(stream.skyOccluder(r->key,bits))continue;
        if(previewCeiling && int64_t(snapshot->slots[id].origin[1])==int64_t(*previewCeiling)*32){
            std::fill_n(masks.begin()+i*32,32,UINT32_MAX);continue;
        }
        if(id==core.slot){std::copy(topOpen.begin(),topOpen.end(),masks.begin()+i*32);continue;}
        const auto it=std::find_if(haloSeeds.begin(),haloSeeds.end(),[&](const auto& seed){
            return seed.handle==ChunkHandle{id,r->generation};});
        if(it!=haloSeeds.end())std::copy(it->top.begin(),it->top.end(),masks.begin()+i*32);
    }
    if(!recordSky(cmd,sources,snapshot->slots,snapshot->selected,bits,masks))return false;
    coreCount_=1;streamCore_={core};streamDataSerials_={stream.dataSerials_[core.slot]};
    streamSky_=true;stream_=&stream;return true;
}
std::optional<VulkanResidentLight::Result> VulkanResidentLight::resultStreamCore(
        VkCommandBuffer cmd,uint32_t frameSlot,const VulkanChunkStreamGpu& stream) const noexcept {
    if(stream_!=&stream || !stream.currentLightFrame(cmd,frameSlot,epoch_))return std::nullopt;
    if(streamDataSerials_.size()!=streamCore_.size())return std::nullopt;
    for(size_t i=0;i<streamCore_.size();++i)
        if(stream.dataSerials_[streamCore_[i].slot]!=streamDataSerials_[i])return std::nullopt;
    auto r=resultRange(epoch_);
    if(!r || !coreCount_)return std::nullopt;
    r->bytes=lightBytes*coreCount_;return r;
}
bool VulkanResidentLight::publishStreamCore(VkCommandBuffer cmd,uint32_t frameSlot,
        VulkanChunkStreamGpu& stream,StreamBatch& changed){
    const auto r=resultStreamCore(cmd,frameSlot,stream);
    if(!r || published_ || changed.streamOwner!=&stream || changed.streamRecordSerial!=epoch_ ||
       streamCore_.size()!=coreCount_ || r->bytes!=lightBytes*streamCore_.size())return false;
    std::vector<VkBufferCopy> copies;
    copies.reserve(streamCore_.size());
    // Allocate/validate everything before recording any GPU write. Mark all
    // neighbouring geometry conservatively (including diagonal light reads).
    auto dirty=changed.remeshSlots;
    const auto size=stream.lightWords_.size();
    std::unique_lock ownership(stream.ownershipMutex_,std::defer_lock);
    if(streamSky_)ownership.lock();
    for(size_t i=0;i<streamCore_.size();++i){
        const auto h=streamCore_[i];
        const auto resident=stream.planner_->slotInfo(h.slot);
        if(!resident || resident->generation!=h.generation || !resident->gpuBlockLightOwned)return false;
        if(streamSky_){
            const auto it=stream.owned_.find(resident->key);
            if(it==stream.owned_.end() || it->second.retiring || it->second.promotion ||
               !stream.matchesOwnedResident(it->second))return false;
        }
        const auto& page=stream.pages_[2].at(h.slot);
        const VkDeviceSize dest=VkDeviceSize(page.value)*4;
        if(!page.active || page.uniform || dest>size || lightBytes>size-dest)return false;
        copies.push_back({r->offset+i*lightBytes,dest,lightBytes});
        for(int x=-1;x<=1;++x)for(int y=-1;y<=1;++y)for(int z=-1;z<=1;++z){
            auto key=resident->key;key.x+=x;key.y+=y;key.z+=z;
            if(auto n=stream.planner_->lookup(key))dirty.push_back(n->slot);
        }
    }
    std::sort(dirty.begin(),dirty.end());dirty.erase(std::unique(dirty.begin(),dirty.end()),dirty.end());
    // The propagation pass read the old arena. Earlier draws and previous
    // same-queue uploads can also access it. Order the overwrite after them.
    dependency(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdCopyBuffer(cmd,r->buffer,stream.lights(),uint32_t(copies.size()),copies.data());
    dependency(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|
                   VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT);
    for(const auto& h:streamCore_)stream.dataSerials_[h.slot]=++stream.nextDataSerial_;
    changed.remeshSlots.swap(dirty);published_=true;return true;
}
std::optional<VulkanResidentLight::Result> VulkanResidentLight::resultCore(uint64_t currentEpoch) const noexcept {
    auto r=result(currentEpoch);
    if(!r||!coreCount_)return std::nullopt;
    r->bytes=lightBytes*coreCount_;return r;
}
bool VulkanResidentLight::recordImpl(VkCommandBuffer cmd,const Sources& s,
                                 std::span<const GpuChunkSlot> slots,std::span<const uint32_t> selected,
                                 std::span<const uint32_t> bits,std::span<const uint8_t> emission,
                                 std::span<const uint32_t> topOpen){
    if(!device_||busy_||!cmd||!s.blocks||!s.seeds||!s.blockBytes||!s.seedBytes||
       s.blockBytes%4||s.seedBytes%4||s.blockBytes>maxRange_||s.seedBytes>maxRange_||
       selected.empty()||selected.size()>maxJobs_||bits.size()!=2048||
       s.blocks==scratch_.buffer()||s.seeds==scratch_.buffer())return false;
    ResidentLightBatch b;
    if(!b.compile<GpuChunkSlot>(slots,selected,s.blockBytes/4,s.seedBytes/4,scratch_.size()/4))return false;
    const bool sky=!topOpen.empty();
    if(sky){
        if(!emission.empty()||topOpen.size()!=selected.size()*32)return false;
        for(size_t j=0;j<b.jobs.size();++j)if(b.jobs[j].neighbor[3]!=ResidentLightBatch::missing){
            const auto mask=topOpen.subspan(j*32,32);
            if(std::any_of(mask.begin(),mask.end(),[](uint32_t v){return v!=0;}))return false;
        }
    }
    // Snapshot only selected slots. The shader uses compact job links, not
    // the original neighbour IDs in the copied slots. No live table reads.
    std::vector<GpuChunkSlot> compact;compact.reserve(selected.size());
    for(uint32_t i=0;i<b.jobs.size();++i){compact.push_back(slots[b.jobs[i].slot]);b.jobs[i].slot=i;}
    slots_.write(0,compact.data(),compact.size()*sizeof(GpuChunkSlot));
    jobs_.write(0,b.jobs.data(),b.jobs.size()*sizeof(ResidentLightBatch::Job));
    transparency_.write(0,bits.data(),bits.size_bytes());
    const bool generated=!emission.empty();
    uint32_t passes=14;
    if(generated){
        std::array<uint32_t,ResidentLightBatch::emissionWords> packed;
        passes=ResidentLightBatch::packEmission(emission,packed);
        transparency_.write(bits.size_bytes(),packed.data(),sizeof(packed));
    }
    if(sky){
        transparency_.write(bits.size_bytes(),topOpen.data(),topOpen.size_bytes());
        passes=std::any_of(topOpen.begin(),topOpen.end(),[](uint32_t w){return w!=0;})?14u:0u;
    }
    const std::array<VkDescriptorBufferInfo,bindings.size()> infos{{
        {s.blocks,0,s.blockBytes},{slots_.buffer(),0,slots_.size()},{jobs_.buffer(),0,jobs_.size()},
        {transparency_.buffer(),0,transparency_.size()},{scratch_.buffer(),0,scratch_.size()},
        {s.seeds,0,s.seedBytes}}};
    std::array<VkWriteDescriptorSet,bindings.size()> writes{};
    for(size_t i=0;i<writes.size();++i){auto& w=writes[i];w={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet=set_;w.dstBinding=bindings[i];w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w.pBufferInfo=&infos[i];}
    vkUpdateDescriptorSets(device_,uint32_t(writes.size()),writes.data(),0,nullptr);
    count_=uint32_t(selected.size());epoch_=s.epoch;busy_=true;
    // HOST_VISIBLE packet writes are flushed by VulkanDeviceBuffer and made
    // available by queue submission. External sources are same-queue GPU writes.
    dependency(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
               VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout_,0,1,&set_,0,nullptr);
    const auto run=[&](const ResidentLightBatch::Params& params){
        vkCmdPushConstants(cmd,pipelineLayout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(params),&params);
        vkCmdDispatch(cmd,params.readBank==4u?4u:128u,count_,1);
    };
    const auto between=[&]{dependency(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);};
    if(sky){run({count_,4u,1u});if(passes)between();}
    else if(generated){run(ResidentLightBatch::seedPass(count_,passes));if(passes)between();}
    for(unsigned p=0;p<passes;++p){
        if(sky){const uint32_t bank=1u^(p&1u);run({count_,5u+bank,bank^1u});}
        else run(generated?ResidentLightBatch::generatedIteration(count_,passes,p):ResidentLightBatch::iteration(count_,p));
        if(p+1<passes)between();
    }
    dependency(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT);
    return true;
}
std::optional<VulkanResidentLight::Result> VulkanResidentLight::result(uint64_t currentEpoch) const noexcept {
    if(stream_)return std::nullopt; // stream transactions require same-command validation
    return resultRange(currentEpoch);
}
std::optional<VulkanResidentLight::Result> VulkanResidentLight::resultRange(uint64_t currentEpoch) const noexcept {
    if(!busy_||currentEpoch!=epoch_)return std::nullopt;
    const auto bytes=lightBytes*count_;
    return Result{scratch_.buffer(),bytes,bytes,epoch_}; // both modes finish in bank 1
}
}
