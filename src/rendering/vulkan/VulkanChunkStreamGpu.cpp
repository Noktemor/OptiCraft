#include "rendering/vulkan/VulkanChunkStreamGpu.h"
#include "rendering/vulkan/VulkanGpuMesher.h"
#include "rendering/vulkan/VulkanMiscMesher.h"

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
    device_=device;
    config_=config;
    try {
        planner_=std::make_unique<VulkanChunkStreamPlan>(config.maxResident,config.maxQueued);
        const auto slots=VkDeviceSize(config.maxResident);
        blockWords_.create(physical,device,slots*kBlockBytes,kStorage,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        metaWords_.create(physical,device,slots*kMetaBytes,kStorage,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        lightWords_.create(physical,device,slots*kLightBytes,kStorage,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
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
StreamBatch VulkanChunkStreamGpu::record(VkCommandBuffer primary,uint32_t frameSlot) {
    if (!device_ || !primary || frameSlot>=kFrameSlots)
        throw std::invalid_argument("VulkanChunkStreamGpu::record invalid frame");
    StreamBatch batch=planner_->take(config_.uploadsPerFrame);
    if (batch.empty() && dirtyGeometry_.empty())return batch;
    // Stage only the changed original voxel data, never the entire resident set.
    stagingBytes_.clear();
    Builder stage{stagingBytes_};
    RangeCopies regions;
    std::vector<uint32_t> drawResets;
    drawResets.reserve(batch.releases.size()+batch.uploads.size());
    for (const auto& release:batch.releases) {
        drawResets.push_back(release.slot);
        opaqueFaces_[release.slot]={};miscFaces_[release.slot]={};
    }
    for (const auto& upload:batch.uploads) {
        const auto slot=upload.slot;
        stage.push(regions.blocks,VkDeviceSize(slot)*kBlockBytes,
                   upload.packet.blockWords.data(),upload.packet.blockWords.size());
        stage.push(regions.meta,VkDeviceSize(slot)*kMetaBytes,
                   upload.packet.metaWords.data(),upload.packet.metaWords.size());
        stage.push(regions.light,VkDeviceSize(slot)*kLightBytes,
                   upload.packet.lightWords.data(),upload.packet.lightWords.size());
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
                           int32_t(int64_t(k.z)*32),0};
            misc.source[0]=slot*kChunkBlockWords;
            misc.source[1]=slot*(kChunkVoxels/4);
            const FaceRange& miscFaces=miscFaces_[slot];
            if(miscFaces.generation==resident->generation){
                misc.source[2]=miscFaces.base;misc.source[3]=miscFaces.count;
            }
            misc.origin=opaque.origin;
            for(int i=0;i<6;i++) {
                const auto key=neighborKey(k,i/2,(i%2==0)?-1:1);
                auto neighbor=key ? planner_->lookup(*key) : std::nullopt;
                const int32_t blockBase=neighbor ? int32_t(neighbor->slot*kChunkBlockWords):-1;
                const int32_t metaBase=neighbor ? int32_t(neighbor->slot*(kChunkVoxels/4)):-1;
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
        std::vector<uint32_t> live;
        live.reserve(planner_->residentCount());
        for(uint32_t i=0;i<config_.maxResident;i++)
            if(planner_->slotInfo(i))live.push_back(i);
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
                         VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    before.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(primary,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&before,0,nullptr,0,nullptr);
    std::sort(drawResets.begin(),drawResets.end());
    drawResets.erase(std::unique(drawResets.begin(),drawResets.end()),drawResets.end());
    for(uint32_t slot:drawResets)
        vkCmdFillBuffer(primary,publishedDraws_.buffer(),
                        VkDeviceSize(slot)*sizeof(VkDrawIndirectCommand),sizeof(VkDrawIndirectCommand),0);
    const VkBuffer src=staging_[frameSlot].buffer.buffer();
    auto copy=[&](const std::vector<VkBufferCopy>& ranges,const VulkanDeviceBuffer& dest) {
        if(!ranges.empty())vkCmdCopyBuffer(primary,src,dest.buffer(),uint32_t(ranges.size()),ranges.data());
    };
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
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,0,1,&after,0,nullptr,0,nullptr);
    return batch;
}
void VulkanChunkStreamGpu::shutdown() noexcept {
    planner_.reset();
    for(auto& frame:staging_)frame.buffer.destroy();
    liveSlots_.destroy();publishedDraws_.destroy();miscSlots_.destroy();
    opaqueSlots_.destroy();lightWords_.destroy();metaWords_.destroy();blockWords_.destroy();
    opaqueFaces_.clear();miscFaces_.clear();dirtyGeometry_.clear();stagingBytes_.clear();
    liveSlotCount_=0;device_=VK_NULL_HANDLE;
}
} // namespace OptiCraft::Vulkan
