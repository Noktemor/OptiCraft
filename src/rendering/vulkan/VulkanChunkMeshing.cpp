#include "rendering/vulkan/VulkanChunkMeshing.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace OptiCraft::Vulkan {
namespace {
constexpr VkBufferUsageFlags kDeviceStorage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|
    VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
VkDeviceSize multiplesOf16(VkDeviceSize n){return (n+15u)&~VkDeviceSize(15u);}
}
void VulkanChunkMeshing::initialize(VkPhysicalDevice physical,VkDevice device,
     VulkanChunkStreamGpu& stream,const VulkanMaterialTables& materials,
     Config config,const Shaders& shaders) {
    if (device_ || !physical || !device || !config.batchLimit ||
        config.batchLimit>1024 || !config.opaqueArenaBytes || !config.miscArenaBytes ||
        config.opaqueArenaBytes%16u || config.miscArenaBytes%32u)
        throw std::invalid_argument("VulkanChunkMeshing invalid device or budgets");
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical,&props);
    const auto maxSSBO=VkDeviceSize(props.limits.maxStorageBufferRange);
    if (config.opaqueArenaBytes>maxSSBO || config.miscArenaBytes>maxSSBO ||
        config.opaqueArenaBytes/8>UINT32_MAX || config.miscArenaBytes/32>UINT32_MAX)
        throw std::invalid_argument("VulkanChunkMeshing geometry exceeds device SSBO range");
    physical_=physical;device_=device;stream_=&stream;
    batchLimit_=config.batchLimit;
    try {
        const uint32_t maxSlots=stream.plan().capacity();
        if (maxSlots>65536u) throw std::invalid_argument("GPU row faces encode at most 65536 stable slots");
        jobs_=std::make_unique<GpuMeshTransactions>(maxSlots,config.opaqueArenaBytes,
                                                    config.miscArenaBytes,config.batchLimit);
        opaqueFaces_.create(physical,device,config.opaqueArenaBytes,kDeviceStorage,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        miscQuads_.create(physical,device,config.miscArenaBytes,kDeviceStorage,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        opaqueScratch_.create(physical,device,multiplesOf16(config.batchLimit*4ull),kDeviceStorage,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        miscScratch_.create(physical,device,multiplesOf16(config.batchLimit*4ull),kDeviceStorage,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        jobSlots_.create(physical,device,multiplesOf16(config.batchLimit*4ull),kDeviceStorage,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        opaqueCandidates_.create(physical,device,multiplesOf16(config.batchLimit*16ull),kDeviceStorage,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        readback_.create(physical,device,2*multiplesOf16(config.batchLimit*4ull),
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        miscDraws_.create(physical,device,VkDeviceSize(maxSlots)*sizeof(VkDrawIndirectCommand),
                          kDeviceStorage|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        GpuMesherBuffers opaqueB{};
        opaqueB.buffers={stream.blocks(),stream.opaqueSlots(),opaqueScratch_.buffer(),
            opaqueFaces_.buffer(),materials.opaqueFlags(),stream.publishedDraws(),
            jobSlots_.buffer(),opaqueCandidates_.buffer(),stream.metadata(),stream.lights()};
        opaqueB.byteSizes={VkDeviceSize(maxSlots)*kChunkBlockWords*4,
            VkDeviceSize(maxSlots)*sizeof(GpuChunkSlot),opaqueScratch_.size(),
            opaqueFaces_.size(),VkDeviceSize(65536)*4,
            VkDeviceSize(maxSlots)*sizeof(VkDrawIndirectCommand),jobSlots_.size(),
            opaqueCandidates_.size(),VkDeviceSize(maxSlots)*kChunkVoxels,
            VkDeviceSize(maxSlots)*kChunkVoxels};
        opaque_.initialize(physical,device,opaqueB,shaders.opaqueCount,shaders.opaqueEmit,
                           shaders.opaqueFinalize,shaders.opaquePublish);
        GpuMiscBuffers miscB{};
        miscB.buffers={stream.blocks(),stream.metadata(),stream.miscSlots(),
            materials.blockDescriptors(),materials.shapeIndex(),materials.shapeBoxes(),
            miscScratch_.buffer(),miscQuads_.buffer(),jobSlots_.buffer(),materials.faceUv(),
            stream.lights()};
        // These sizes are the actual buffer sizes from the catalogue, not
        // approximate material counts (shape boxes may be variable).
        miscB.byteSizes={VkDeviceSize(maxSlots)*kChunkBlockWords*4,
            VkDeviceSize(maxSlots)*kChunkVoxels,multiplesOf16(VkDeviceSize(maxSlots)*sizeof(GpuMiscSlot)),
            materials.blockDescriptorBytes(),materials.shapeIndexBytes(),
            materials.shapeBoxBytes(),miscScratch_.size(),miscQuads_.size(),
            jobSlots_.size(),materials.faceUvBytes(),VkDeviceSize(maxSlots)*kChunkVoxels};
        misc_.initialize(physical,device,miscB,shaders.miscCount,shaders.miscEmit);
    }catch(...){shutdown();throw;}
}
void VulkanChunkMeshing::beginFrame(uint32_t frameSlot) {
    if (!device_ || frameSlot>=VulkanChunkStreamGpu::kFrameSlots)
        throw std::invalid_argument("VulkanChunkMeshing::beginFrame invalid slot");
    jobs_->completedFrame();
    if (frameSlot!=waitingSlot_ || waiting_==Wait::None) return;
    const size_t count=jobs_->jobs().size();
    readOpaque_.resize(count);readMisc_.resize(count);
    if(count) {
        readback_.read(0,readOpaque_.data(),count*4);
        readback_.read(multiplesOf16(batchLimit_*4ull),readMisc_.data(),count*4);
    }
    if(waiting_==Wait::Count) {
        if (jobs_->finishCount(readOpaque_,readMisc_)) {
            for(const auto& j:jobs_->jobs()) {
                if(!j.eligible || !matching(j))continue;
                stream_->setOpaqueFaceRange(j.slot,j.generation,
                    uint32_t(j.opaqueCandidate.offset/8),j.opaqueCount);
                stream_->setMiscQuadRange(j.slot,j.generation,
                    uint32_t(j.miscCandidate.offset/32),j.miscCount);
            }
        }
        waiting_=Wait::None;
    } else if (waiting_==Wait::Emit) waiting_=Wait::PublishReady;
    waitingSlot_=UINT32_MAX;
}
bool VulkanChunkMeshing::matching(const GpuMeshTransactions::Entry& j)const {
    auto cur=stream_->plan().slotInfo(j.slot);
    return cur && cur->generation==j.generation;
}
void VulkanChunkMeshing::barrier(VkCommandBuffer cmd,VkPipelineStageFlags src,
        VkAccessFlags sa,VkPipelineStageFlags dst,VkAccessFlags da)const {
    VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    memory.srcAccessMask=sa;memory.dstAccessMask=da;
    vkCmdPipelineBarrier(cmd,src,dst,0,1,&memory,0,nullptr,0,nullptr);
}
std::vector<uint32_t> VulkanChunkMeshing::batchIndices(bool eligibleOnly)const {
    std::vector<uint32_t> ids;ids.reserve(jobs_->jobs().size());
    for(const auto& j:jobs_->jobs())
        ids.push_back((eligibleOnly && (!j.eligible || !matching(j)))?UINT32_MAX:j.slot);
    return ids;
}
void VulkanChunkMeshing::uploadJobs(VkCommandBuffer cmd,std::span<const uint32_t> ids) {
    if(ids.empty())return;
    if(ids.size()>batchLimit_ || ids.size()*4>65536)
        throw std::out_of_range("GPU jobs too large for vkCmdUpdateBuffer");
    barrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdUpdateBuffer(cmd,jobSlots_.buffer(),0,VkDeviceSize(ids.size())*4,ids.data());
    barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT);
}
void VulkanChunkMeshing::recordReadback(VkCommandBuffer cmd){
    // GPU count SSBOs have already issued shader -> transfer barriers.
    const VkBufferCopy opaqueCopy{0,0,VkDeviceSize(jobs_->jobs().size())*4};
    const VkBufferCopy miscCopy{0,multiplesOf16(batchLimit_*4ull),opaqueCopy.size};
    vkCmdCopyBuffer(cmd,opaqueScratch_.buffer(),readback_.buffer(),1,&opaqueCopy);
    vkCmdCopyBuffer(cmd,miscScratch_.buffer(),readback_.buffer(),1,&miscCopy);
    barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT);
}
void VulkanChunkMeshing::record(VkCommandBuffer cmd,uint32_t frameSlot,
                                const StreamBatch& changed){
    if (!device_ || !cmd || frameSlot>=VulkanChunkStreamGpu::kFrameSlots)
        throw std::invalid_argument("VulkanChunkMeshing::record invalid command buffer");
    // Changes/revisions must be captured before any publish validation.
    for(const auto& dead:changed.releases)jobs_->evict(dead.slot,dead.generation);
    // On a recycled slot, the old misc indirect command is just as invalid
    // as the opaque command which the Stage5 stream already resets.
    for(const auto& dead:changed.releases)
        vkCmdFillBuffer(cmd,miscDraws_.buffer(),VkDeviceSize(dead.slot)*16,16,0);
    for(const auto& upload:changed.uploads)
        if(upload.resetDraw)
            vkCmdFillBuffer(cmd,miscDraws_.buffer(),VkDeviceSize(upload.slot)*16,16,0);
    if(!changed.releases.empty() ||
        std::any_of(changed.uploads.begin(),changed.uploads.end(),
                    [](const StreamUpload& u){return u.resetDraw;}))
        barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
    for(uint32_t slot:changed.remeshSlots){
        auto current=stream_->plan().slotInfo(slot);
        if(current)jobs_->dirty(slot,current->generation);
    }
    if (waiting_==Wait::PublishReady) {
        auto changes=jobs_->verifyEmit(readOpaque_,readMisc_);
        std::vector<uint32_t> ids;ids.reserve(changes.size());
        for(const auto& change:changes)
            ids.push_back(change.valid?change.slot:UINT32_MAX);
        // Inactive jobs use sentinel and cannot overwrite a newly recycled slot.
        uploadJobs(cmd,ids);
        opaque_.recordPublish(cmd,uint32_t(ids.size()));
        for(const auto& change:changes) {
            if(!change.valid)continue;
            VkDrawIndirectCommand indirect{6,change.miscCount,0,change.miscBase};
            vkCmdUpdateBuffer(cmd,miscDraws_.buffer(),VkDeviceSize(change.slot)*16,16,&indirect);
        }
        barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_INDIRECT_COMMAND_READ_BIT|VK_ACCESS_SHADER_READ_BIT);
        jobs_->acceptPublish(changes);
        waiting_=Wait::None;
    }
    if (waiting_!=Wait::None)return; // one scratch/readback batch at once
    if (jobs_->phase()==GpuMeshTransactions::Phase::Emitting) {
        const auto indices=batchIndices(true);
        uploadJobs(cmd,indices);
        opaque_.recordEmit(cmd,uint32_t(indices.size()));
        misc_.recordEmit(cmd,uint32_t(indices.size()));
        recordReadback(cmd);
        waiting_=Wait::Emit;waitingSlot_=frameSlot;
    } else if(jobs_->startCount()){
        const auto indices=batchIndices(false);
        uploadJobs(cmd,indices);
        opaque_.recordCount(cmd,uint32_t(indices.size()));
        misc_.recordCount(cmd,uint32_t(indices.size()));
        recordReadback(cmd);
        waiting_=Wait::Count;waitingSlot_=frameSlot;
    }
}
void VulkanChunkMeshing::shutdown()noexcept{
    // Only legal after all submissions have finished (renderer shutdown).
    misc_.shutdown();opaque_.shutdown();
    miscDraws_.destroy();readback_.destroy();opaqueCandidates_.destroy();
    jobSlots_.destroy();miscScratch_.destroy();opaqueScratch_.destroy();
    miscQuads_.destroy();opaqueFaces_.destroy();
    jobs_.reset();device_=VK_NULL_HANDLE;physical_=VK_NULL_HANDLE;stream_=nullptr;
    waiting_=Wait::None;waitingSlot_=UINT32_MAX;
    batchLimit_=0;readOpaque_.clear();readMisc_.clear();
}
} // namespace OptiCraft::Vulkan
