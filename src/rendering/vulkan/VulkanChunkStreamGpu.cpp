#include "rendering/vulkan/VulkanChunkStreamGpu.h"
#include "rendering/vulkan/VulkanGpuMesher.h"
#include "rendering/vulkan/VulkanMiscMesher.h"
#include "rendering/vulkan/SurfaceLod.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_set>

namespace OptiCraft::Vulkan {
namespace {
constexpr VkDeviceSize kBlockBytes=VkDeviceSize(kChunkBlockWords)*sizeof(uint32_t);
constexpr VkDeviceSize kMetaBytes=VkDeviceSize(kChunkVoxels/4)*sizeof(uint32_t);
constexpr VkDeviceSize kLightBytes=kMetaBytes;
constexpr std::array<VkDeviceSize,3> kChannelBytes{kBlockBytes,kMetaBytes,kLightBytes};
constexpr VkBufferUsageFlags kStorage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|
    VK_BUFFER_USAGE_TRANSFER_DST_BIT;
constexpr VkBufferUsageFlags kIndirect = kStorage|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
// Only the uploader sees the CPU bytes. Staging is always per-frame and reused
// after that frame's VkFence has signalled.
struct Builder {
    std::vector<std::byte>& data;
    VkDeviceSize append(const void* source, VkDeviceSize bytes) {
        const VkDeviceSize start=(data.size()+3u)&~VkDeviceSize(3);
        if (bytes>SIZE_MAX-start) throw std::overflow_error("Staging buffer offset overflow");
        data.resize(size_t(start+bytes));
        if (bytes) std::memcpy(data.data()+start,source,size_t(bytes));
        return start;
    }
    template<typename T> void push(std::vector<VkBufferCopy>& list,
                                   VkDeviceSize dest, const T* source,size_t count) {
        if (!count) return;
        const auto bytes=VkDeviceSize(count)*sizeof(T);
        list.push_back({append(source,bytes),dest,bytes});
    }
};
std::optional<ChunkKey> neighborKey(ChunkKey k,int axis,int sign) {
    int64_t v=axis==0?k.x:axis==1?k.y:k.z;
    v+=sign;
    if(v<INT32_MIN||v>INT32_MAX)return std::nullopt;
    if(axis==0)k.x=int32_t(v);else if(axis==1)k.y=int32_t(v);else k.z=int32_t(v);
    return k;
}
}
void VulkanChunkStreamGpu::initialize(VkPhysicalDevice physical,VkDevice device,Config config) {
    if (device_ || !physical || !device || !config.maxResident ||
        config.maxResident>4096 || !config.uploadsPerFrame ||
        config.uploadsPerFrame>config.maxResident || !config.maxQueued)
        throw std::invalid_argument("VulkanChunkStreamGpu: invalid config");
    device_=device;physical_=physical;
    config_=config;
    try {
        planner_=std::make_unique<VulkanChunkStreamPlan>(config.maxResident,config.maxQueued);
        const auto slots=VkDeviceSize(config.maxResident);
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physical,&props);
        std::array<VulkanDeviceBuffer*,3> buffers{&blockWords_,&metaWords_,&lightWords_};
        for(size_t k=0;k<buffers.size();++k){
            const bool dynamic=config.indirectVoxelPages && config.growVoxelArenas;
            // Independent logical slots must not double the previous raw voxel
            // budget. Inline channels use no physical pages.
            const auto pageLimit=dynamic?std::min<VkDeviceSize>(slots,2048u):slots;
            maxPages_[k]=uint32_t(std::min<VkDeviceSize>(pageLimit,props.limits.maxStorageBufferRange/kChannelBytes[k]));
            if(!maxPages_[k] || (!dynamic && maxPages_[k]<slots))
                throw std::invalid_argument("Voxel arena exceeds storage buffer range");
            const uint32_t initial=dynamic?std::min(maxPages_[k],std::max(1u,config.initialPages[k])):config.maxResident;
            buffers[k]->create(physical,device,VkDeviceSize(initial)*kChannelBytes[k],
                kStorage|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if(config.indirectVoxelPages)pages_[k].reset(config.maxResident,uint32_t(kChannelBytes[k]/4),initial);
        }
        opaqueSlots_.create(physical,device,slots*sizeof(GpuChunkSlot),kStorage,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        miscSlots_.create(physical,device,slots*sizeof(GpuMiscSlot),kStorage,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        publishedDraws_.create(physical,device,slots*sizeof(VkDrawIndirectCommand),kIndirect,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        liveSlots_.create(physical,device,slots*sizeof(uint32_t),kStorage,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        const VkDeviceSize stagingCapacity = VkDeviceSize(config.uploadsPerFrame)*
            (kBlockBytes+kMetaBytes+kLightBytes)+slots*
            (sizeof(GpuChunkSlot)+sizeof(GpuMiscSlot)+sizeof(uint32_t))+256;
        for(auto& frame:staging_)frame.buffer.create(physical,device,stagingCapacity,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        opaqueFaces_.resize(config.maxResident);
        miscFaces_.resize(config.maxResident);
        repeated_.resize(config.maxResident);
        dirtyGeometry_.reserve(config.maxResident);
        stagingBytes_.reserve(size_t(stagingCapacity));
    } catch (...) {shutdown();throw;}
}
void VulkanChunkStreamGpu::submit(std::vector<LiveChunkPacket>&& packets) {
    if (!planner_) throw std::logic_error("VulkanChunkStreamGpu not initialized");
    planner_->submit(std::move(packets));
}
void VulkanChunkStreamGpu::evict(ChunkKey key) {
    if (!planner_) throw std::logic_error("VulkanChunkStreamGpu not initialized");
    planner_->evict(key);
}
VulkanGameChunkFeed::UploadSink VulkanChunkStreamGpu::uploadSink() {
    return [this](std::vector<LiveChunkPacket>&& packets){ submit(std::move(packets)); };
}
VulkanGameChunkFeed::EvictSink VulkanChunkStreamGpu::evictSink() {
    return [this](ChunkKey key){ evict(key); };
}
void VulkanChunkStreamGpu::setOpaqueFaceRange(uint32_t slot,uint32_t generation,
                                              uint32_t base,uint32_t capacity) {
    const auto resident=planner_ && slot<opaqueFaces_.size() ? planner_->slotInfo(slot) : std::nullopt;
    if (!resident || resident->generation!=generation)
        throw std::invalid_argument("Stale opaque geometry range");
    opaqueFaces_[slot]={generation,base,capacity};
    dirtyGeometry_.push_back(slot);
}
void VulkanChunkStreamGpu::setMiscQuadRange(uint32_t slot,uint32_t generation,
                                            uint32_t base,uint32_t capacity) {
    const auto resident=planner_ && slot<miscFaces_.size() ? planner_->slotInfo(slot) : std::nullopt;
    if (!resident || resident->generation!=generation)
        throw std::invalid_argument("Stale misc geometry range");
    miscFaces_[slot]={generation,base,capacity};
    dirtyGeometry_.push_back(slot);
}
uint64_t VulkanChunkStreamGpu::voxelAllocatedBytes() const noexcept {
    uint64_t bytes=blockWords_.allocationBytes()+metaWords_.allocationBytes()+lightWords_.allocationBytes();
    for(const auto& frame:retiredVoxels_)for(const auto& b:frame)bytes+=b.allocationBytes();
    return bytes;
}
bool VulkanChunkStreamGpu::growChannel(size_t kind,uint32_t needed,uint32_t frameSlot){
    auto& pool=pages_[kind];
    if(needed<=pool.capacity())return true;
    auto& retired=retiredVoxels_[frameSlot][kind];
    if(!config_.growVoxelArenas || needed>maxPages_[kind] || retired || retryFrames_[kind])return false;
    const uint32_t target=std::min(maxPages_[kind],std::max({needed,pool.capacity()*2u,needed+config_.uploadsPerFrame-1u}));
    VulkanDeviceBuffer next;
    auto allocate=[&](uint32_t n){
        try{next.create(physical_,device_,VkDeviceSize(n)*kChannelBytes[kind],
            kStorage|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);return true;}
        catch(const VulkanBufferError& e){
            if(e.result!=VK_ERROR_OUT_OF_DEVICE_MEMORY && e.result!=VK_ERROR_OUT_OF_HOST_MEMORY)throw;
            return false;
        }
    };
    uint32_t capacity=target;
    if(!allocate(capacity)){
        capacity=needed;
        if(capacity==target || !allocate(capacity)){retryFrames_[kind]=30;return false;}
    }
    auto& current=kind==0?blockWords_:kind==1?metaWords_:lightWords_;
    retired=std::move(current);current=std::move(next);
    pool.grow(capacity);++voxelEpoch_;++growthCount_;
    return true;
}
void VulkanChunkStreamGpu::tryShrinkChannels(uint32_t frameSlot,bool idle){
    if(!config_.indirectVoxelPages || !config_.growVoxelArenas || !config_.shrinkDelayFrames)return;
    for(size_t k=0;k<3;++k){
        auto& pool=pages_[k];
        const uint32_t floor=std::min(maxPages_[k],std::max(1u,config_.initialPages[k]));
        const uint32_t cap=pool.capacity();
        // Hysteresis: do not resize for temporary empty slots during streaming.
        if(!idle || cap<=floor || pool.densePages()>cap/4u || retiredVoxels_[frameSlot][k]){
            idleFrames_[k]=0;continue;
        }
        if(++idleFrames_[k]<config_.shrinkDelayFrames)continue;
        idleFrames_[k]=0;
        if(retryFrames_[k])continue;
        const uint32_t target=std::max({floor,cap/2u,pool.usedExtent()});
        // Require a meaningful reduction; no relocation of live pages.
        if(target>cap-cap/4u || target>=cap)continue;
        VulkanDeviceBuffer next;
        try{next.create(physical_,device_,VkDeviceSize(target)*kChannelBytes[k],
            kStorage|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);}
        catch(const VulkanBufferError& e){
            if(e.result!=VK_ERROR_OUT_OF_DEVICE_MEMORY && e.result!=VK_ERROR_OUT_OF_HOST_MEMORY)throw;
            retryFrames_[k]=30;continue;
        }
        auto& current=k==0?blockWords_:k==1?metaWords_:lightWords_;
        retiredVoxels_[frameSlot][k]=std::move(current);current=std::move(next);
        pool.shrink(target);++voxelEpoch_;++shrinkCount_;
    }
}
StreamBatch VulkanChunkStreamGpu::record(VkCommandBuffer primary,uint32_t frameSlot,bool allowUploads) {
    if (!device_ || !primary || frameSlot>=kFrameSlots)
        throw std::invalid_argument("VulkanChunkStreamGpu::record invalid frame");
    // beginFrame has waited this slot's fence; all earlier queue work and the
    // previous migration using these source buffers have completed.
    for(auto& old:retiredVoxels_[frameSlot])old.destroy();
    for(auto& cooldown:retryFrames_)if(cooldown)--cooldown;
    std::array<uint32_t,3> planned{};
    for(size_t k=0;k<3;++k)planned[k]=pages_[k].densePages();
    VulkanChunkStreamPlan::Admission admit;
    if(config_.indirectVoxelPages && config_.growVoxelArenas){
        admit=[&](uint32_t slot,const LiveChunkPacket& packet,bool uploadVoxels){
            if(!uploadVoxels)return true;
            const std::array<bool,3> dense{packet.blockWords.size()!=1,packet.metaWords.size()!=1,packet.lightWords.size()!=1};
            auto next=planned;
            for(size_t k=0;k<3;++k){
                const auto& old=pages_[k].at(slot);
                // A committed inline transition frees its old page before
                // later uploads. All eviction pages were credited before admission.
                const bool wasDense=old.active && !old.uniform;
                if(dense[k] && !wasDense)++next[k];
                else if(!dense[k] && wasDense)--next[k];
                if(next[k]>pages_[k].capacity() && !growChannel(k,next[k],frameSlot)){
                    ++admissionMisses_;return false;
                }
            }
            planned=next;return true;
        };
    }
    // Credit *all* evictions before admission, not only reused slot IDs.
    VulkanChunkStreamPlan::ReleaseNotice onRelease;
    if(config_.indirectVoxelPages)onRelease=[&](uint32_t slot){
        for(size_t k=0;k<3;++k){
            pages_[k].release(slot);planned[k]=pages_[k].densePages();
        }
    };
    StreamBatch batch=planner_->take(allowUploads?config_.uploadsPerFrame:0u,admit,onRelease);
    tryShrinkChannels(frameSlot,batch.uploads.empty() && batch.releases.empty() && planner_->pendingCount()==0);
    const bool migrating=std::any_of(retiredVoxels_[frameSlot].begin(),retiredVoxels_[frameSlot].end(),
        [](const auto& b){return bool(b);});
    if (batch.empty() && dirtyGeometry_.empty() && !migrating)return batch;
    // Stage only the changed original voxel data, never the entire resident set.
    stagingBytes_.clear();
    Builder stage{stagingBytes_};
    RangeCopies regions;
    std::array<std::vector<Fill>,3> voxelFills;
    auto channel=[&](uint32_t slot,size_t kind,VkDeviceSize bytes,
                     const std::vector<uint32_t>& words,std::vector<VkBufferCopy>& copies){
        if(config_.indirectVoxelPages){
            auto& pool=pages_[kind];
            const auto old=pool.at(slot);
            const auto next=pool.set(slot,words.size()==1,words[0]);
            if(next.uniform){
                if(old.active && old.uniform && old.value==next.value)uploadStats_.reusedBytes+=bytes;
                else uploadStats_.inlineBytes+=bytes;
            }else{
                stage.push(copies,VkDeviceSize(next.value)*4,words.data(),words.size());
                uploadStats_.copiedBytes+=bytes;
            }
            return;
        }
        auto& old=repeated_[slot][kind];
        const auto dest=VkDeviceSize(slot)*bytes;
        if(words.size()==1){
            if(old.known && old.word==words[0]) uploadStats_.reusedBytes+=bytes;
            else {
                voxelFills[kind].push_back({dest,bytes,words[0]});
                uploadStats_.filledBytes+=bytes;
            }
            old={words[0],true};
        }else{
            stage.push(copies,dest,words.data(),words.size());
            uploadStats_.copiedBytes+=bytes;
            old={};
        }
    };
    std::vector<uint32_t> drawResets;
    drawResets.reserve(batch.releases.size()+batch.uploads.size());
    for (const auto& release:batch.releases) {
        drawResets.push_back(release.slot);
        opaqueFaces_[release.slot]={};miscFaces_[release.slot]={};
        repeated_[release.slot]={};
        // Page ownership was already released by onRelease, before admission.
    }
    for (const auto& upload:batch.uploads) {
        const auto slot=upload.slot;
        if(upload.resetDraw){
            repeated_[slot]={};
            if(config_.indirectVoxelPages)for(auto& pool:pages_)pool.release(slot);
        }
        if(upload.uploadVoxels){
            channel(slot,0,kBlockBytes,upload.packet.blockWords,regions.blocks);
            channel(slot,1,kMetaBytes,upload.packet.metaWords,regions.meta);
            channel(slot,2,kLightBytes,upload.packet.lightWords,regions.light);
        }
        if(upload.resetDraw) {
            drawResets.push_back(slot);
            opaqueFaces_[slot]={};miscFaces_[slot]={};
        }
    }
    // Neighbor origins/offsets are metadata. The compute shader still performs
    // every visibility test; the CPU never evaluates a face.
    std::unordered_set<uint32_t> affected(batch.remeshSlots.begin(),batch.remeshSlots.end());
    affected.insert(dirtyGeometry_.begin(),dirtyGeometry_.end());
    dirtyGeometry_.clear();
    for(uint32_t slot:affected) {
        auto resident=planner_->slotInfo(slot);
        GpuChunkSlot opaque{};
        GpuMiscSlot misc{};
        if(resident) {
            const auto& k=resident->key;
            opaque.source[0]=slot*kChunkBlockWords;
            opaque.source[3]=1;
            const FaceRange& faces=opaqueFaces_[slot];
            if(faces.generation==resident->generation) {
                opaque.source[1]=faces.base;opaque.source[2]=faces.count;
            }
            opaque.origin={int32_t(int64_t(k.x)*32),int32_t(int64_t(k.y)*32),
                           int32_t(int64_t(k.z)*32),int32_t((resident->meshKinds&(7u|kLodFlags))|8u)};
            misc.source[0]=slot*kChunkBlockWords;
            misc.source[1]=slot*(kChunkVoxels/4);
            const FaceRange& miscFaces=miscFaces_[slot];
            if(miscFaces.generation==resident->generation){
                misc.source[2]=miscFaces.base;misc.source[3]=miscFaces.count;
            }
            if(config_.indirectVoxelPages){
                const auto& block=pages_[0].at(slot);
                const auto& meta=pages_[1].at(slot);
                const auto& light=pages_[2].at(slot);
                if(!block.active || !meta.active || !light.active)throw std::logic_error("Missing voxel channel");
                opaque.source[0]=misc.source[0]=block.value;
                misc.source[1]=meta.value;
                opaque.neighborB[2]=int32_t(meta.value);
                opaque.neighborB[3]=misc.neighborBlocksB[2]=int32_t(light.value);
                opaque.origin[3]|=int32_t(kVoxelAddressFlag|
                    (block.uniform?kVoxelUniformBlock:0u)|(meta.uniform?kVoxelUniformMeta:0u)|
                    (light.uniform?kVoxelUniformLight:0u));
            }
            misc.origin=opaque.origin;
            for(int i=0;i<6;i++) {
                const auto key=neighborKey(k,i/2,(i%2==0)?-1:1);
                auto neighbor=key ? planner_->lookup(*key) : std::nullopt;
                const int32_t blockBase=neighbor ? int32_t(config_.indirectVoxelPages?neighbor->slot:neighbor->slot*kChunkBlockWords):-1;
                const int32_t metaBase=neighbor ? int32_t(config_.indirectVoxelPages?neighbor->slot:neighbor->slot*(kChunkVoxels/4)):-1;
                if(i<4){
                    opaque.neighborA[i]=blockBase;
                    misc.neighborBlocksA[i]=blockBase;
                    misc.neighborMetaA[i]=metaBase;
                }else{
                    opaque.neighborB[i-4]=blockBase;
                    misc.neighborBlocksB[i-4]=blockBase;
                    misc.neighborMetaB[i-4]=metaBase;
                }
            }
        }
        stage.push(regions.opaque,VkDeviceSize(slot)*sizeof(GpuChunkSlot),&opaque,1);
        stage.push(regions.misc,VkDeviceSize(slot)*sizeof(GpuMiscSlot),&misc,1);
    }
    // An ordered dense slot list feeds GPU frustum culling. Rebuild only on
    // topology changes (new chunk/evict), not for each camera movement/frame.
    const bool topologyChanged=!batch.releases.empty() ||
        std::any_of(batch.uploads.begin(),batch.uploads.end(),
                    [](const StreamUpload& upload){ return upload.newResident; });
    if(topologyChanged) {
        const auto live=planner_->liveSlotsSnapshot();
        liveSlotCount_=uint32_t(live.size());
        stage.push(regions.live,0,live.data(),live.size());
    }
    if(stagingBytes_.size()>staging_[frameSlot].buffer.size())
        throw std::overflow_error("Vulkan stream staging budget exceeded");
    if(!stagingBytes_.empty())staging_[frameSlot].buffer.write(0,stagingBytes_.data(),stagingBytes_.size());
    // Draws and source buffers may be read by older submissions on this SAME
    // Vulkan queue. Ordered memory dependency before overwriting resident data.
    VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    before.srcAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|
                         VK_ACCESS_INDIRECT_COMMAND_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
    before.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT|(migrating?VK_ACCESS_TRANSFER_READ_BIT:0u);
    vkCmdPipelineBarrier(primary,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT|
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&before,0,nullptr,0,nullptr);
    if(migrating){
        const auto targets=voxelBuffers();
        for(size_t k=0;k<3;++k){
            const auto& old=retiredVoxels_[frameSlot][k];
            if(old){const VkBufferCopy region{0,0,std::min(old.size(),voxelSizes()[k])};
                vkCmdCopyBuffer(primary,old.buffer(),targets[k],1,&region);
                uploadStats_.migratedBytes+=region.size;}
        }
        // Subsequent uploads may overwrite copied pages: explicit WAW dependency.
        VkMemoryBarrier copied{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        copied.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;copied.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(primary,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,1,&copied,0,nullptr,0,nullptr);
    }
    std::sort(drawResets.begin(),drawResets.end());
    drawResets.erase(std::unique(drawResets.begin(),drawResets.end()),drawResets.end());
    for(uint32_t slot:drawResets)
        vkCmdFillBuffer(primary,publishedDraws_.buffer(),
                        VkDeviceSize(slot)*sizeof(VkDrawIndirectCommand),sizeof(VkDrawIndirectCommand),0);
    const VkBuffer src=staging_[frameSlot].buffer.buffer();
    auto copy=[&](const std::vector<VkBufferCopy>& ranges,const VulkanDeviceBuffer& dest) {
        if(!ranges.empty())vkCmdCopyBuffer(primary,src,dest.buffer(),uint32_t(ranges.size()),ranges.data());
    };
    // Each slot/channel has at most one operation per batch. Fill and copy
    // ranges are disjoint; existing transfer barriers cover both commands.
    const std::array<VkBuffer,3> targets{blocks(),metadata(),lights()};
    for(size_t kind=0;kind<targets.size();++kind)
        for(const auto& fill:voxelFills[kind])
            vkCmdFillBuffer(primary,targets[kind],fill.offset,fill.size,fill.word);
    copy(regions.blocks,blockWords_);
    copy(regions.meta,metaWords_);
    copy(regions.light,lightWords_);
    copy(regions.opaque,opaqueSlots_);
    copy(regions.misc,miscSlots_);
    copy(regions.live,liveSlots_);
    VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    after.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    after.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|
                        VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    vkCmdPipelineBarrier(primary,VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,0,1,&after,0,nullptr,0,nullptr);
    return batch;
}
void VulkanChunkStreamGpu::shutdown() noexcept {
    planner_.reset();
    for(auto& frame:retiredVoxels_)for(auto& b:frame)b.destroy();
    for(auto& frame:staging_)frame.buffer.destroy();
    liveSlots_.destroy();publishedDraws_.destroy();miscSlots_.destroy();
    opaqueSlots_.destroy();lightWords_.destroy();metaWords_.destroy();blockWords_.destroy();
    opaqueFaces_.clear();miscFaces_.clear();dirtyGeometry_.clear();stagingBytes_.clear();
    repeated_.clear();for(auto& pool:pages_)pool.reset(0,1);uploadStats_={};
    liveSlotCount_=0;device_=VK_NULL_HANDLE;physical_=VK_NULL_HANDLE;
    maxPages_={};retryFrames_={};idleFrames_={};voxelEpoch_=growthCount_=shrinkCount_=admissionMisses_=0;
}
} // namespace OptiCraft::Vulkan
