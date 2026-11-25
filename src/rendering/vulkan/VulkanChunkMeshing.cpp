#include "rendering/vulkan/VulkanChunkMeshing.h"
#include "rendering/vulkan/SurfaceLod.h"
#include <algorithm>
#include "rendering/vulkan/MeshDispatchRuns.h"
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
        config.batchLimit>1024 || !config.activeBatches || config.activeBatches>kBatchCount || !config.opaqueArenaBytes || !config.miscArenaBytes ||
        config.opaqueArenaBytes%16u || config.miscArenaBytes%32u)
        throw std::invalid_argument("VulkanChunkMeshing invalid device or budgets");
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical,&props);
    const auto maxSSBO=VkDeviceSize(props.limits.maxStorageBufferRange);
    if (config.opaqueArenaBytes>maxSSBO || config.miscArenaBytes>maxSSBO ||
        config.opaqueArenaBytes/8>UINT32_MAX || config.miscArenaBytes/32>UINT32_MAX)
        throw std::invalid_argument("VulkanChunkMeshing geometry exceeds device SSBO range");
    physical_=physical;device_=device;stream_=&stream;
    batchLimit_=config.batchLimit;activeBatches_=config.activeBatches;fastOpaque_=config.fastOpaque;fastMiscQuads_=config.fastMiscQuads;
    // Keep the bounded cache within the device storage range. A zero capacity
    // uses the original two-pass mesher (including on constrained devices).
    const uint64_t cacheLimit=maxSSBO/config.batchLimit;
    // Preserve the configured rectangle cache; disable only the coarse scratch
    // if the device cannot fit both within its storage descriptor range.
    opaqueCacheRects_=cacheLimit>20?uint32_t(std::min<uint64_t>(config.opaqueCacheRects,(cacheLimit-20)/8)):0u;
    const uint64_t rectBytes=4ull+8ull*opaqueCacheRects_;
    const bool coarseCache=config.coarseCache && cacheLimit>=rectBytes+4ull*kCoarseWordsPerJob+16u;
    const bool planeCompaction=config.planeCompaction && coarseCache &&
        cacheLimit>=rectBytes+4ull*(kCoarseWordsPerJob+kPlaneWordsPerJob)+16u;
    const auto cacheBytes=(opaqueCacheRects_||coarseCache)?
        multiplesOf16(VkDeviceSize(config.batchLimit)*(rectBytes+(coarseCache?4ull*kCoarseWordsPerJob:0ull)+
            (planeCompaction?4ull*kPlaneWordsPerJob:0ull))):16u;
    try {
        const uint32_t maxSlots=stream.plan().capacity();
        if (maxSlots>65536u) throw std::invalid_argument("GPU row faces encode at most 65536 stable slots");
        jobs_=std::make_unique<GpuMeshTransactions>(maxSlots,config.opaqueArenaBytes,
                                                    config.miscArenaBytes,config.batchLimit,config.workBudget,activeBatches_);
        opaqueFaces_.create(physical,device,config.opaqueArenaBytes,kDeviceStorage,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        miscQuads_.create(physical,device,config.miscArenaBytes,kDeviceStorage,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        miscDraws_.create(physical,device,VkDeviceSize(maxSlots)*sizeof(VkDrawIndirectCommand),
                          kDeviceStorage|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        for(uint32_t i=0;i<activeBatches_;++i){
            auto& batch=batches_[i];
            batch.opaqueScratch.create(physical,device,multiplesOf16(config.batchLimit*4ull),kDeviceStorage,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            batch.miscScratch.create(physical,device,multiplesOf16(config.batchLimit*4ull),kDeviceStorage,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            batch.jobSlots.create(physical,device,multiplesOf16(config.batchLimit*4ull),kDeviceStorage,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            batch.opaqueCandidates.create(physical,device,multiplesOf16(config.batchLimit*16ull),kDeviceStorage,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            batch.readback.create(physical,device,2*multiplesOf16(config.batchLimit*4ull)+config.batchLimit*16ull,
                             VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            batch.opaqueCache.create(physical,device,cacheBytes,
                kDeviceStorage|(planeCompaction?VkBufferUsageFlags(VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT):0u),VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            GpuMesherBuffers opaqueB{};
            opaqueB.cacheRectsPerJob=opaqueCacheRects_;
            opaqueB.coarseCache=coarseCache;
            opaqueB.planeCompaction=planeCompaction;
            opaqueB.buffers={stream.blocks(),stream.opaqueSlots(),batch.opaqueScratch.buffer(),
                opaqueFaces_.buffer(),materials.opaqueFlags(),stream.publishedDraws(),
                batch.jobSlots.buffer(),batch.opaqueCandidates.buffer(),stream.metadata(),stream.lights(),batch.opaqueCache.buffer()};
            opaqueB.byteSizes={stream.voxelSizes()[0],
                VkDeviceSize(maxSlots)*sizeof(GpuChunkSlot),batch.opaqueScratch.size(),
                opaqueFaces_.size(),VkDeviceSize(65536)*4,
                VkDeviceSize(maxSlots)*sizeof(VkDrawIndirectCommand),batch.jobSlots.size(),
                batch.opaqueCandidates.size(),stream.voxelSizes()[1],
                stream.voxelSizes()[2],batch.opaqueCache.size()};
            batch.opaque.initialize(physical,device,opaqueB,shaders.opaqueCount,shaders.opaqueEmit,
                               shaders.opaqueFinalize,shaders.opaquePublish);
            GpuMiscBuffers miscB{};
            miscB.buffers={stream.blocks(),stream.metadata(),stream.miscSlots(),
                materials.blockDescriptors(),materials.shapeIndex(),materials.shapeBoxes(),
                batch.miscScratch.buffer(),miscQuads_.buffer(),batch.jobSlots.buffer(),materials.faceUv(),
                stream.lights()};
            // These sizes are the actual buffer sizes from the catalogue, not
            // approximate material counts (shape boxes may be variable).
            miscB.byteSizes={stream.voxelSizes()[0],
                stream.voxelSizes()[1],multiplesOf16(VkDeviceSize(maxSlots)*sizeof(GpuMiscSlot)),
                materials.blockDescriptorBytes(),materials.shapeIndexBytes(),
                materials.shapeBoxBytes(),batch.miscScratch.size(),miscQuads_.size(),
                batch.jobSlots.size(),materials.faceUvBytes(),stream.voxelSizes()[2]};
            batch.misc.initialize(physical,device,miscB,shaders.miscCount,shaders.miscEmit);
        }
    }catch(...){shutdown();throw;}
}
void VulkanChunkMeshing::selectVoxelFrame(uint32_t frameSlot){
    for(uint32_t i=0;i<activeBatches_;++i){
        auto& b=batches_[i];
        b.opaque.selectVoxelFrame(frameSlot,stream_->voxelEpoch(),stream_->voxelBuffers(),stream_->voxelSizes());
        b.misc.selectVoxelFrame(frameSlot,stream_->voxelEpoch(),stream_->voxelBuffers(),stream_->voxelSizes());
    }
}
void VulkanChunkMeshing::beginFrame(uint32_t frameSlot) {
    if (!device_ || frameSlot>=VulkanChunkStreamGpu::kFrameSlots)
        throw std::invalid_argument("VulkanChunkMeshing::beginFrame invalid slot");
    jobs_->completedFrame();
    for(uint32_t i=0;i<activeBatches_;++i){
        auto& batch=batches_[i];
        if(frameSlot!=batch.waitingSlot || batch.waiting==Wait::None)continue;
        jobs_->selectBatch(i);
        const size_t count=jobs_->jobs().size();
        batch.readOpaque.resize(count);batch.readMisc.resize(count);
        if(count){
            batch.readback.read(0,batch.readOpaque.data(),count*4);
            if(batch.waiting==Wait::FastOpaque){
                if(batch.fastMixed)batch.readback.read(multiplesOf16(batchLimit_*4ull),batch.readMisc.data(),count*4);
                else std::fill(batch.readMisc.begin(),batch.readMisc.end(),0u);
                batch.readFastDraws.resize(count);
                batch.readback.read(2*multiplesOf16(batchLimit_*4ull),batch.readFastDraws.data(),count*16);
            }else batch.readback.read(multiplesOf16(batchLimit_*4ull),batch.readMisc.data(),count*4);
        }
        if(batch.waiting==Wait::Count){
            batch.emitReady=jobs_->finishCount(batch.readOpaque,batch.readMisc);
            if(batch.emitReady)for(const auto& j:jobs_->jobs()){
                if(!j.eligible || !matching(j))continue;
                stream_->setOpaqueFaceRange(j.slot,j.generation,uint32_t(j.opaqueCandidate.offset/8),j.opaqueCount);
                stream_->setMiscQuadRange(j.slot,j.generation,uint32_t(j.miscCandidate.offset/32),j.miscCount);
            }
            batch.waiting=Wait::None;
        }else if(batch.waiting==Wait::FastOpaque){
            jobs_->finishFastOpaque(batch.readOpaque,batch.readFastDraws,
                batch.fastMixed?std::span<const uint32_t>(batch.readMisc):std::span<const uint32_t>{});
            batch.fastMixed=false;
            batch.waiting=Wait::PublishReady;
        }else if(batch.waiting==Wait::Emit)batch.waiting=Wait::PublishReady;
        batch.waitingSlot=UINT32_MAX;
    }
}
bool VulkanChunkMeshing::acceptsUploads() const noexcept {
    if(!jobs_ || jobs_->pendingCount()>=size_t(batchLimit_)*2u)return false;
    // Give an allocated batch a stable source snapshot for its imminent Emit.
    // Uploads can proceed during Count/Emit fence latency, not on Emit frames.
    for(const auto& batch:batches_)if(batch.emitReady)return false;
    return true;
}
bool VulkanChunkMeshing::mayDispatch(const StreamBatch& changed) const noexcept {
    if(jobs_->hasPending() || !changed.remeshSlots.empty())return true;
    for(const auto& batch:batches_)if(batch.emitReady)return true;
    return false;
}
bool VulkanChunkMeshing::matching(const GpuMeshTransactions::Entry& j)const {
    auto cur=stream_->plan().slotInfo(j.slot);
    return cur && cur->generation==j.generation && jobs_->isCurrent(j);
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
std::vector<uint8_t> VulkanChunkMeshing::batchKinds(std::span<const uint32_t> ids)const{
    std::vector<uint8_t> kinds; kinds.reserve(ids.size());
    for(const auto id:ids){
        const auto slot=stream_->plan().slotInfo(id);
        const uint32_t flags=slot?slot->meshKinds:0u;
        const bool coarse=(flags&(kLod2Flags|1u))==(kLod2Flags|1u) && !(flags&6u);
        kinds.push_back(uint8_t(flags&31u)|(coarse?kCoarseOpaqueMesh:0u));
    }
    return kinds;
}
void VulkanChunkMeshing::uploadJobs(Batch& batch,VkCommandBuffer cmd,std::span<const uint32_t> ids) {
    if(ids.empty())return;
    if(ids.size()>batchLimit_ || ids.size()*4>65536)
        throw std::out_of_range("GPU jobs too large for vkCmdUpdateBuffer");
    barrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdUpdateBuffer(cmd,batch.jobSlots.buffer(),0,VkDeviceSize(ids.size())*4,ids.data());
    barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT);
}
void VulkanChunkMeshing::recordReadback(Batch& batch,VkCommandBuffer cmd){
    // GPU count SSBOs have already issued shader -> transfer barriers.
    const VkBufferCopy opaqueCopy{0,0,VkDeviceSize(jobs_->jobs().size())*4};
    const VkBufferCopy miscCopy{0,multiplesOf16(batchLimit_*4ull),opaqueCopy.size};
    vkCmdCopyBuffer(cmd,batch.opaqueScratch.buffer(),batch.readback.buffer(),1,&opaqueCopy);
    vkCmdCopyBuffer(cmd,batch.miscScratch.buffer(),batch.readback.buffer(),1,&miscCopy);
    barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT);
}
uint32_t VulkanChunkMeshing::record(VkCommandBuffer cmd,uint32_t frameSlot,
                                const StreamBatch& changed){
    if (!device_ || !cmd || frameSlot>=VulkanChunkStreamGpu::kFrameSlots)
        throw std::invalid_argument("VulkanChunkMeshing::record invalid command buffer");
    lastDispatchWasCount_=false;
    // Published draws are shared across frame slots. A previous frame may
    // still read them in culling; order reads before clear/publish writes.
    if(!changed.remeshSlots.empty() || !changed.releases.empty() ||
       std::any_of(batches_.begin(),batches_.end(),[](const Batch& b){return b.waiting==Wait::PublishReady;}))
        barrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_INDIRECT_COMMAND_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_SHADER_WRITE_BIT);
    // Changes/revisions must be captured before any publish validation.
    for(const auto& dead:changed.releases)jobs_->evict(dead.slot,dead.generation);
    // On a recycled slot, the old misc indirect command is just as invalid
    // as the opaque command which the Stage5 stream already resets.
    std::vector<uint32_t> miscResets;
    miscResets.reserve(changed.releases.size()+changed.uploads.size());
    for(const auto& dead:changed.releases)miscResets.push_back(dead.slot);
    for(const auto& upload:changed.uploads)if(upload.resetDraw)miscResets.push_back(upload.slot);
    std::sort(miscResets.begin(),miscResets.end());
    miscResets.erase(std::unique(miscResets.begin(),miscResets.end()),miscResets.end());
    for(uint32_t slot:miscResets)
        vkCmdFillBuffer(cmd,miscDraws_.buffer(),VkDeviceSize(slot)*16,16,0);
    if(!miscResets.empty())
        barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_INDIRECT_COMMAND_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT);
    for(uint32_t slot:changed.remeshSlots){
        auto current=stream_->plan().slotInfo(slot);
        if(!current)continue;
        const auto kinds=current->meshKinds;
        if((kinds&3u)==0u || ((kinds&7u)==5u && stream_->plan().fullyOccluded(slot))){
            // Exact empty or sealed opaque draw: no GPU Count/Emit or readback needed.
            jobs_->clearEmpty(slot,current->generation);
            // A released/recycled slot was already zeroed by stream.record and above.
            if(!std::binary_search(miscResets.begin(),miscResets.end(),slot)){
                vkCmdFillBuffer(cmd,stream_->publishedDraws(),VkDeviceSize(slot)*16,16,0);
                vkCmdFillBuffer(cmd,miscDraws_.buffer(),VkDeviceSize(slot)*16,16,0);
            }
        }else{
            const uint32_t cost=(kinds&4u)?1u:((kinds&1u)?((kinds&kFarGeometryLodFlag)?24u:((kinds&kGeometryLodFlag)?16u:8u)):0u)+((kinds&2u)?4u:0u);
            jobs_->dirty(slot,current->generation,cost,
                std::binary_search(changed.interactiveRemeshSlots.begin(),changed.interactiveRemeshSlots.end(),slot));
        }
    }
    if(!changed.remeshSlots.empty())barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
    // Publish all completed lanes before scheduling one new compute phase.
    // Reusing a lane in the same command buffer is safe after its fence and
    // uploadJobs' compute-read -> transfer-write dependency.
    for(uint32_t i=0;i<activeBatches_;++i){
        auto& batch=batches_[i];
        if(batch.waiting!=Wait::PublishReady)continue;
        jobs_->selectBatch(i);
        auto changes=jobs_->verifyEmit(batch.readOpaque,batch.readMisc);
        std::vector<uint32_t> ids;ids.reserve(changes.size());
        for(const auto& change:changes)ids.push_back(change.valid?change.slot:UINT32_MAX);
        uploadJobs(batch,cmd,ids);
        batch.opaque.recordPublish(cmd,uint32_t(ids.size()));
        for(const auto& change:changes){
            if(!change.valid)continue;
            VkDrawIndirectCommand indirect{6,change.miscCount,0,change.miscBase};
            vkCmdUpdateBuffer(cmd,miscDraws_.buffer(),VkDeviceSize(change.slot)*16,16,&indirect);
        }
        barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_INDIRECT_COMMAND_READ_BIT|VK_ACCESS_SHADER_READ_BIT);
        jobs_->acceptPublish(changes);
        batch.waiting=Wait::None;
    }
    // Complete interactive Emit first. If an idle scratch lane exists, let
    // a new edit overtake background Emit once, then drain that Emit to avoid
    // starving allocations on continuous edits. Never overwrite active scratch.
    bool readyInteractiveEmit=false,readyBackgroundEmit=false,idleLane=false;
    for(uint32_t i=0;i<activeBatches_;++i){
        const auto& b=batches_[i];
        if(b.emitReady){if(jobs_->batchInteractive(i))readyInteractiveEmit=true;else readyBackgroundEmit=true;}
        else if(b.waiting==Wait::None)idleLane=true;
    }
    const bool bypassBackgroundEmit=!readyInteractiveEmit && readyBackgroundEmit && idleLane &&
        !deferredBackgroundEmit_ && jobs_->hasReadyInteractive();
    for(uint32_t offset=0;offset<activeBatches_;++offset){
        const uint32_t i=(nextBatch_+offset)%activeBatches_;
        auto& batch=batches_[i];
        if(!batch.emitReady || (readyInteractiveEmit && !jobs_->batchInteractive(i)) || bypassBackgroundEmit)continue;
        jobs_->selectBatch(i);
        const auto indices=batchIndices(true);
        uploadJobs(batch,cmd,indices);
        auto kinds=batchKinds(indices);
        if(opaqueCacheRects_)for(size_t job=0;job<kinds.size();++job){
            if(!(kinds[job]&1u))continue;
            const bool cached=jobs_->jobs()[job].opaqueCount<=opaqueCacheRects_;
            jobs_->recordOpaqueCacheDispatch(cached);
            if(cached)kinds[job]|=kCachedOpaqueMesh;
        }
        batch.opaque.recordEmit(cmd,uint32_t(indices.size()),kinds);
        batch.misc.recordEmit(cmd,uint32_t(indices.size()),kinds);
        recordReadback(batch,cmd);
        batch.emitReady=false;batch.waiting=Wait::Emit;batch.waitingSlot=frameSlot;
        if(!jobs_->batchInteractive(i))deferredBackgroundEmit_=false;
        nextBatch_=(i+1)%activeBatches_;
        return jobs_->batchWork(true);
    }
    for(uint32_t offset=0;offset<activeBatches_;++offset){
        const uint32_t i=(nextBatch_+offset)%activeBatches_;
        auto& batch=batches_[i];
        if(batch.waiting!=Wait::None || batch.emitReady)continue;
        jobs_->selectBatch(i);
        if(!jobs_->startCount())continue;
        if(bypassBackgroundEmit)deferredBackgroundEmit_=true;
        const auto indices=batchIndices(false);
        uploadJobs(batch,cmd,indices);
        const auto kinds=batchKinds(indices);
        const bool pureOpaque=std::all_of(kinds.begin(),kinds.end(),[](uint8_t k){return (k&3u)==1u;});
        const auto slab=fastOpaque_ && (pureOpaque || fastMiscQuads_)?
            jobs_->reserveFastOpaque(opaqueCacheRects_,pureOpaque?0u:fastMiscQuads_):BufferRange{};
        batch.opaque.recordCount(cmd,uint32_t(indices.size()),kinds);
        if(slab){
            // No slot range update or CPU Count readback between these dispatches.
            // Only the final counts/indirect candidates are returned after the fence.
            // Misc Emit uses a private bounded range, and counts even overflow.
            batch.opaque.recordFastOpaque(cmd,uint32_t(indices.size()),slab);
            const auto miscSlab=jobs_->fastMiscSlab();
            batch.fastMixed=bool(miscSlab);
            if(miscSlab)batch.misc.recordFastEmit(cmd,uint32_t(indices.size()),kinds,miscSlab,fastMiscQuads_);
            const VkBufferCopy countCopy{0,0,VkDeviceSize(indices.size())*4};
            const VkBufferCopy drawCopy{0,2*multiplesOf16(batchLimit_*4ull),VkDeviceSize(indices.size())*16};
            vkCmdCopyBuffer(cmd,batch.opaqueScratch.buffer(),batch.readback.buffer(),1,&countCopy);
            vkCmdCopyBuffer(cmd,batch.opaqueCandidates.buffer(),batch.readback.buffer(),1,&drawCopy);
            if(batch.fastMixed){
                const VkBufferCopy miscCopy{0,multiplesOf16(batchLimit_*4ull),countCopy.size};
                vkCmdCopyBuffer(cmd,batch.miscScratch.buffer(),batch.readback.buffer(),1,&miscCopy);
            }
            barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT);
            batch.waiting=Wait::FastOpaque;
        }else{
            batch.misc.recordCount(cmd,uint32_t(indices.size()),kinds);
            recordReadback(batch,cmd);
            batch.waiting=Wait::Count;
        }
        batch.waitingSlot=frameSlot;
        nextBatch_=(i+1)%activeBatches_;
        lastDispatchWasCount_=true;
        return jobs_->batchWork(false);
    }
    return 0;
}
void VulkanChunkMeshing::shutdown()noexcept{
    // Only legal after all submissions have finished (renderer shutdown).
    for(auto& batch:batches_){
        batch.misc.shutdown();batch.opaque.shutdown();
        batch.readback.destroy();batch.opaqueCandidates.destroy();batch.opaqueCache.destroy();
        batch.jobSlots.destroy();batch.miscScratch.destroy();batch.opaqueScratch.destroy();
        batch.waiting=Wait::None;batch.waitingSlot=UINT32_MAX;batch.emitReady=false;batch.fastMixed=false;
        batch.readOpaque.clear();batch.readMisc.clear();batch.readFastDraws.clear();
    }
    miscDraws_.destroy();miscQuads_.destroy();opaqueFaces_.destroy();
    jobs_.reset();device_=VK_NULL_HANDLE;physical_=VK_NULL_HANDLE;stream_=nullptr;
    lastDispatchWasCount_=false;deferredBackgroundEmit_=false;
    batchLimit_=0;nextBatch_=0;activeBatches_=0;opaqueCacheRects_=0;fastMiscQuads_=0;
}
} // namespace OptiCraft::Vulkan
