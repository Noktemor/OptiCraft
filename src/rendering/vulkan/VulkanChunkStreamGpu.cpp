#include "rendering/vulkan/VulkanChunkStreamGpu.h"
#include "rendering/vulkan/VulkanGpuMesher.h"
#include "rendering/vulkan/VulkanMiscMesher.h"
#include "rendering/vulkan/SurfaceLod.h"
#include "rendering/vulkan/LightVerticalBounds.h"

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
        skyRoofs_.resize(config.maxResident);
        dataSerials_.resize(config.maxResident);
        dirtyGeometry_.reserve(config.maxResident);
        stagingBytes_.reserve(size_t(stagingCapacity));
    } catch (...) {shutdown();throw;}
}
void VulkanChunkStreamGpu::submit(std::vector<LiveChunkPacket>&& packets) {
    if (!planner_) throw std::logic_error("VulkanChunkStreamGpu not initialized");
    if(!config_.indirectVoxelPages && std::any_of(packets.begin(),packets.end(),
        [](const auto& p){return p.gpuBlockLightOwned;}))
        throw std::invalid_argument("GPU block-light ownership requires indirect voxel pages");
    for(const auto& packet:packets)if(packet.gpuSourceLease &&
        acquireOverlay(packet.overlayReference,device_)!=packet.gpuSourceLease)
        throw LodControlSourceMissing("Vulkan stream: GPU source lease invalid");
    std::lock_guard lock(ownershipMutex_);
    for(const auto& packet:packets)if(owned_.contains(packet.position))
        throw LodControlSourceMissing("Vulkan stream: exclusive GPU world source");
    planner_->submit(std::move(packets));
}
bool VulkanChunkStreamGpu::submitGpuSnapshot(LiveChunkPacket&& packet) {
    if(!planner_)throw std::logic_error("VulkanChunkStreamGpu not initialized");
    if(packet.lodOnly || packet.lightOnly || packet.gpuSourceLease ||
       !packet.blockWords.empty() || packet.metaWords.size()>1 ||
       (packet.gpuBlockLightOwned && !config_.indirectVoxelPages))
        throw std::invalid_argument("Invalid GPU source-only packet");
    auto source=acquireOverlay(packet.overlayReference,device_);
    if(!source)return false;
    packet.gpuSourceLease=std::move(source);
    std::vector<LiveChunkPacket> batch;
    batch.push_back(std::move(packet));
    submit(std::move(batch));
    return true;
}
uint64_t VulkanChunkStreamGpu::allocateOwnershipSerial() noexcept {
    auto old=ownershipSerial_.load(std::memory_order_relaxed);
    while(old!=UINT64_MAX){
        if(ownershipSerial_.compare_exchange_weak(old,old+1,std::memory_order_relaxed))return old+1;
    }
    return 0;
}
std::optional<VulkanChunkStreamGpu::OwnershipToken>
VulkanChunkStreamGpu::submitOwnedGpuSnapshot(LiveChunkPacket&& packet) {
    if(!planner_ || !config_.indirectVoxelPages || packet.lodOnly || packet.lightOnly ||
       packet.gpuSourceLease || !packet.blockWords.empty() || packet.metaWords.size()>1 ||
       !packet.chunkInstance)return std::nullopt;
    auto source=acquireOverlay(packet.overlayReference,device_);
    if(!source)return std::nullopt;
    std::lock_guard lock(ownershipMutex_);
    const auto key=packet.position;
    if(owned_.size()>=config_.maxResident || owned_.contains(key) ||
       planner_->lookup(key) || planner_->hasPendingChange(key))
        return std::nullopt;
    const auto serial=allocateOwnershipSerial();if(!serial)return std::nullopt;
    const OwnershipToken token{this,key,serial};
    owned_.emplace(key,OwnedEntry{token,packet.chunkInstance,{},false});
    try {
        packet.gpuSourceLease=std::move(source);
        std::vector<LiveChunkPacket> batch;
        batch.push_back(std::move(packet));
        planner_->submit(std::move(batch));
    } catch(...) {owned_.erase(key);throw;}
    return token;
}
bool VulkanChunkStreamGpu::matchesOwnedResident(const OwnedEntry& entry) const {
    if(!planner_)return false;
    const auto resident=planner_->lookup(entry.token.key);
    return resident && resident->instance==entry.instance &&
        !planner_->hasPendingChange(entry.token.key);
}
std::optional<VulkanChunkStreamGpu::OwnershipState>
VulkanChunkStreamGpu::ownershipState(OwnershipToken token) const {
    std::lock_guard lock(ownershipMutex_);
    const auto it=owned_.find(token.key);
    if(it==owned_.end() || it->second.token!=token)return std::nullopt;
    const auto& e=it->second;
    if(e.retiring)return OwnershipState::Retiring;
    if(e.promotion)return OwnershipState::Promoting;
    return matchesOwnedResident(e)?OwnershipState::Resident:OwnershipState::Queued;
}
std::optional<VulkanChunkStreamGpu::OwnershipState>
VulkanChunkStreamGpu::ownedStateAt(ChunkKey key) const {
    std::lock_guard lock(ownershipMutex_);
    const auto it=owned_.find(key);
    if(it==owned_.end())return std::nullopt;
    const auto& e=it->second;
    if(e.retiring)return OwnershipState::Retiring;
    if(e.promotion)return OwnershipState::Promoting;
    return matchesOwnedResident(e)?OwnershipState::Resident:OwnershipState::Queued;
}
std::optional<VulkanChunkStreamGpu::PromotionToken>
VulkanChunkStreamGpu::beginOwnedPromotion(OwnershipToken token,uint64_t cpuTicket) {
    std::lock_guard lock(ownershipMutex_);
    auto it=owned_.find(token.key);
    if(!cpuTicket || it==owned_.end() || it->second.token!=token || it->second.retiring ||
       it->second.promotion || !matchesOwnedResident(it->second))
        return std::nullopt;
    size_t active=0;
    for(const auto& [key,e]:owned_)if(e.promotion){
        if(e.promotion.cpuTicket==cpuTicket)return std::nullopt;
        ++active;
    }
    if(active>=kFrameSlots)return std::nullopt;
    const auto serial=allocateOwnershipSerial();if(!serial)return std::nullopt;
    it->second.promotion={token,serial,cpuTicket};
    return it->second.promotion;
}
bool VulkanChunkStreamGpu::cancelOwnedPromotion(PromotionToken token) {
    std::lock_guard lock(ownershipMutex_);
    auto it=owned_.find(token.source.key);
    if(!token || it==owned_.end() || it->second.promotion!=token)return false;
    it->second.promotion={};
    return true;
}
bool VulkanChunkStreamGpu::discardOwnedChunk(OwnershipToken token) {
    std::lock_guard lock(ownershipMutex_);
    auto it=owned_.find(token.key);
    if(it==owned_.end() || it->second.token!=token || it->second.promotion || it->second.retiring)
        return false;
    it->second.retiring=true;
    return true;
}
void VulkanChunkStreamGpu::retireOwnedIdle() {
    if(!planner_)return;
    std::lock_guard lock(ownershipMutex_);
    for(const auto& [key,e]:owned_)planner_->retireForReplacement(key);
    owned_.clear();
    for(auto& frame:readbacks_)if(frame.snapshot && frame.snapshot->promotion){frame.snapshot.reset();frame.ready=false;}
    ++lightFrameSerial_;lightFrameCmd_=VK_NULL_HANDLE;lightFrameSlot_=UINT32_MAX;
}
void VulkanChunkStreamGpu::evict(ChunkKey key) {
    if (!planner_) throw std::logic_error("VulkanChunkStreamGpu not initialized");
    std::lock_guard lock(ownershipMutex_);
    if(!owned_.contains(key))planner_->evict(key);
}
VulkanGameChunkFeed::UploadSink VulkanChunkStreamGpu::uploadSink() {
    return [this](std::vector<LiveChunkPacket>&& packets){ submit(std::move(packets)); };
}
VulkanGameChunkFeed::EvictSink VulkanChunkStreamGpu::evictSink() {
    return [this](ChunkKey key){ evict(key); };
}
VulkanGameChunkFeed::SnapshotLeaseSink VulkanChunkStreamGpu::snapshotLeaseSink() {
    return [this](const OverlayReference& ref){return acquireOverlay(ref,device_);};
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
bool VulkanChunkStreamGpu::currentLightFrame(VkCommandBuffer cmd,uint32_t frameSlot,uint64_t serial) const noexcept {
    return device_ && config_.indirectVoxelPages && cmd && cmd==lightFrameCmd_ &&
        frameSlot==lightFrameSlot_ && serial==lightFrameSerial_;
}
bool VulkanChunkStreamGpu::uniformSkyOccluder(ChunkKey key,std::span<const uint32_t> skyBits) const {
    if(!planner_ || !config_.indirectVoxelPages || skyBits.size()!=2048)return false;
    const auto r=planner_->lookup(key);if(!r)return false;
    const auto& page=pages_[0].at(r->slot);
    if(!page.active || !page.uniform)return false;
    const uint32_t a=page.value&65535u,b=page.value>>16u;
    return !(skyBits[a/32u]&(1u<<(a%32u))) && !(skyBits[b/32u]&(1u<<(b%32u)));
}
bool VulkanChunkStreamGpu::skyOccluder(ChunkKey key,std::span<const uint32_t> skyBits) const {
    if(!planner_ || !config_.indirectVoxelPages || skyBits.size()!=2048)return false;
    const auto r=planner_->lookup(key);if(!r)return false;
    const auto& page=pages_[0].at(r->slot);
    if(!page.active)return false;
    if(page.uniform)return uniformSkyOccluder(key,skyBits);
    const auto& roof=skyRoofs_[r->slot];
    if(!roof.count)return false;
    for(uint8_t i=0;i<roof.count;++i){const auto id=roof.ids[i];
        if(skyBits[id/32u]&(1u<<(id%32u)))return false;
    }
    return true;
}
std::optional<VulkanChunkStreamGpu::LightFrameSnapshot> VulkanChunkStreamGpu::lightFrameSnapshot(
        VkCommandBuffer cmd,uint32_t frameSlot,std::span<const ChunkHandle> core,uint32_t maxJobs,
        std::optional<int32_t> skyCeiling,bool coreOnly,bool partialHalo,std::span<const uint32_t> skyBits) const {
    if((partialHalo && (coreOnly || core.size()!=1 || maxJobs<27)) ||
       !currentLightFrame(cmd,frameSlot,lightFrameSerial_) || core.empty() || core.size()>maxJobs)
        return std::nullopt;
    const LightVerticalBounds bounds{worldLightBounds.low,skyCeiling.value_or(worldLightBounds.high)};
    if(bounds.high>worldLightBounds.high || bounds.high<bounds.low)return std::nullopt;
    LightFrameSnapshot result;
    result.serial=lightFrameSerial_;
    result.slots.resize(planner_->capacity());
    result.core.reserve(core.size());
    std::unordered_set<uint32_t> seenCore;
    std::optional<int32_t> dimension;
    std::vector<StreamResident> centers;
    centers.reserve(core.size());
    for(const auto& h:core){
        auto r=planner_->slotInfo(h.slot);
        if(!h || !r || r->generation!=h.generation || !seenCore.insert(h.slot).second ||
           (dimension && *dimension!=r->key.dimension))return std::nullopt;
        if(!bounds.contains(r->key.y))return std::nullopt;
        dimension=r->key.dimension;
        centers.push_back(*r);result.core.push_back(h.slot);
    }
    // Capture only the required halo, using current stable-slot/page ownership.
    // Producer submit/evict only queues requests; record() is the sole committer.
    result.selected=result.core;
    uint32_t captured=0;
    const auto capture=[&](ChunkKey k){
        const auto r=planner_->lookup(k);
        if(!r)return false;
        auto& s=result.slots[r->slot];
        if(s.source[3])return true;
        if(++captured>maxJobs)return false;
        if(!seenCore.contains(r->slot))result.selected.push_back(r->slot);
        const auto& block=pages_[0].at(r->slot);
        const auto& light=pages_[2].at(r->slot);
        if(!block.active || !light.active)return false;
        s.source={block.value,0,0,1};
        s.origin={int32_t(int64_t(k.x)*32),int32_t(int64_t(k.y)*32),int32_t(int64_t(k.z)*32),
            int32_t(kVoxelAddressFlag|(block.uniform?kVoxelUniformBlock:0u)|(light.uniform?kVoxelUniformLight:0u))};
        s.neighborB[3]=int32_t(light.value);
        for(int f=0;f<6;++f){
            const auto key=neighborKey(k,f/2,(f&1)?1:-1);
            const auto n=key?planner_->lookup(*key):std::nullopt;
            const int32_t id=n?int32_t(n->slot):-1;
            if(f<4)s.neighborA[f]=id;else s.neighborB[f-4]=id;
        }
        return true;
    };
    if(coreOnly){
        for(const auto& c:centers)if(!capture(c.key))return std::nullopt;
        return result;
    }
    for(const auto& c:centers)for(int x=-1;x<=1;++x)for(int y=-1;y<=1;++y)for(int z=-1;z<=1;++z){
        auto k=c.key;k.x+=x;k.y+=y;k.z+=z;
        if(!bounds.contains(k.y))continue;
        if(partialHalo && !planner_->lookup(k))continue;
        if(!capture(k))return std::nullopt;
    }
    // Provisional sky: extend contiguous upper chains a layer at a time.
    // Never jump a missing page or exceed the existing scratch budget.
    // Certified opaque top planes terminate chains just as in exact sky.
    // Partial results are render-only, even if some columns reach the ceiling.
    if(partialHalo && skyCeiling){
        const auto center=centers.front().key;
        std::array<bool,9> open{};
        for(int x=-1;x<=1;++x)for(int z=-1;z<=1;++z){
            auto k=center;k.x+=x;k.z+=z;++k.y;
            open[size_t((x+1)*3+z+1)]=k.y<=*skyCeiling && bool(planner_->lookup(k)) && !skyOccluder(k,skyBits);
        }
        for(int64_t y=int64_t(center.y)+2;y<=*skyCeiling && captured<maxJobs;++y){
            bool any=false;
            for(int x=-1;x<=1 && captured<maxJobs;++x)for(int z=-1;z<=1 && captured<maxJobs;++z){
                auto& active=open[size_t((x+1)*3+z+1)];if(!active)continue;
                auto k=center;k.x+=x;k.z+=z;k.y=int32_t(y);
                if(!planner_->lookup(k)){active=false;continue;}
                if(!capture(k))return std::nullopt;
                if(skyOccluder(k,skyBits))active=false;
                any=true;
            }
            if(!any)break;
        }
        return result;
    }
    // Only direct sky can originate more than one chunk above a core.
    // A committed opaque top plane cuts off that column, but never skip any
    // mandatory halo page. Unknown pages require the usual ceiling chain.
    if(skyCeiling){
        for(const auto& c:centers)for(int x=-1;x<=1;++x)for(int z=-1;z<=1;++z){
            auto k=c.key;k.x+=x;k.z+=z;k.y=int32_t(std::min<int64_t>(int64_t(k.y)+1,*skyCeiling));
            while(k.y<*skyCeiling && !skyOccluder(k,skyBits)){
                ++k.y;
                if(!capture(k))return std::nullopt;
            }
        }
    }
    return result;
}
bool VulkanChunkStreamGpu::recordResidentReadback(VkCommandBuffer cmd,uint32_t frameSlot,ChunkHandle handle){
    if(!device_ || !cmd || cmd!=lightFrameCmd_ || frameSlot>=kFrameSlots ||
       frameSlot!=lightFrameSlot_ || !handle)return false;
    auto& frame=readbacks_[frameSlot];
    if(frame.snapshot)return false;
    const auto resident=planner_->slotInfo(handle.slot);
    if(!resident || resident->generation!=handle.generation)return false;
    ResidentReadback snapshot;
    snapshot.owner=this;snapshot.source=*resident;snapshot.dataSerial=dataSerials_[handle.slot];
    std::array<VkBufferCopy,3> regions{};
    const auto sizes=voxelSizes();
    VkDeviceSize offset=0;
    bool dense=false;
    for(size_t k=0;k<3;++k){
        const auto bytes=kChannelBytes[k];
        const auto page=config_.indirectVoxelPages?pages_[k].at(handle.slot):
            VoxelChannelPages::Entry{uint32_t(VkDeviceSize(handle.slot)*bytes/4),false,true};
        if(!page.active)return false;
        if(page.uniform)snapshot.words[k]={page.value};
        else {
            const auto src=VkDeviceSize(page.value)*4;
            if(src>sizes[k] || bytes>sizes[k]-src)return false;
            regions[k]={src,offset,bytes};
            snapshot.words[k].resize(size_t(bytes/4));
            dense=true;
        }
        offset+=bytes;
    }
    if(dense && !frame.buffer){
        try{frame.buffer.create(physical_,device_,offset,VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_CACHED_BIT);}
        catch(const VulkanBufferError& e){
            if(e.result==VK_ERROR_OUT_OF_DEVICE_MEMORY || e.result==VK_ERROR_OUT_OF_HOST_MEMORY)return false;
            throw;
        }
    }
    if(dense){
        VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        before.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        before.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&before,0,nullptr,0,nullptr);
        const auto buffers=voxelBuffers();
        for(size_t k=0;k<3;++k)if(regions[k].size)
            vkCmdCopyBuffer(cmd,buffers[k],frame.buffer.buffer(),1,&regions[k]);
        VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        after.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        after.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
            0,1,&after,0,nullptr,0,nullptr);
    }
    frame.snapshot=std::move(snapshot);frame.ready=false;
    return true;
}
std::optional<VulkanChunkStreamGpu::ResidentReadback> VulkanChunkStreamGpu::takeResidentReadback(uint32_t frameSlot){
    if(frameSlot>=kFrameSlots)return std::nullopt;
    auto& frame=readbacks_[frameSlot];
    if(!frame.ready || !frame.snapshot)return std::nullopt;
    VkDeviceSize offset=0;
    for(size_t k=0;k<3;++k){
        auto& words=frame.snapshot->words[k];
        if(words.size()!=1)frame.buffer.read(offset,words.data(),kChannelBytes[k]);
        offset+=kChannelBytes[k];
    }
    auto result=std::move(frame.snapshot);frame.snapshot.reset();frame.ready=false;
    return result;
}
std::optional<VulkanChunkStreamGpu::ResidentReadback>
VulkanChunkStreamGpu::takeOwnedReadback(uint32_t frameSlot,PromotionToken token){
    if(!token || token.source.owner!=this || frameSlot>=kFrameSlots)return std::nullopt;
    const auto& frame=readbacks_[frameSlot];
    if(!frame.snapshot || frame.snapshot->promotion!=token)return std::nullopt;
    return takeResidentReadback(frameSlot);
}
bool VulkanChunkStreamGpu::residentReadbackCurrent(const ResidentReadback& result) const {
    if(!planner_ || result.owner!=this)return false;
    const auto r=planner_->slotInfo(result.source.slot);
    return r && r->key==result.source.key && r->generation==result.source.generation &&
        r->instance==result.source.instance && result.dataSerial!=0 &&
        dataSerials_[r->slot]==result.dataSerial;
}
bool VulkanChunkStreamGpu::residentReadbackQuiescent(const ResidentReadback& result) const {
    return residentReadbackCurrent(result) && !planner_->hasPendingChange(result.source.key);
}
bool VulkanChunkStreamGpu::recordOwnedReadback(VkCommandBuffer cmd,uint32_t frameSlot,PromotionToken token){
    std::lock_guard lock(ownershipMutex_);
    const auto it=owned_.find(token.source.key);
    if(!token || it==owned_.end() || it->second.promotion!=token || !matchesOwnedResident(it->second))
        return false;
    const auto r=planner_->lookup(token.source.key);
    if(!r || !recordResidentReadback(cmd,frameSlot,{r->slot,r->generation}))return false;
    readbacks_[frameSlot].snapshot->promotion=token;
    return true;
}
bool VulkanChunkStreamGpu::commitOwnedPromotion(PromotionToken token,const ResidentReadback& snapshot,
                                               const std::function<bool()>& commitCpu){
    {
        std::lock_guard lock(ownershipMutex_);
        const auto it=owned_.find(token.source.key);
        if(!token || !commitCpu || it==owned_.end() || it->second.promotion!=token ||
           snapshot.promotion!=token || snapshot.source.key!=token.source.key || snapshot.source.instance!=it->second.instance ||
           !residentReadbackQuiescent(snapshot))return false;
    }
    // Owner-thread callback: ordinary producer submissions remain excluded.
    if(!commitCpu())return false;
    std::lock_guard lock(ownershipMutex_);
    auto& e=owned_.at(token.source.key);
    e.promotion={};e.retiring=true;
    return true;
}
StreamBatch VulkanChunkStreamGpu::record(VkCommandBuffer primary,uint32_t frameSlot,bool allowUploads) {
    if (!device_ || !primary || frameSlot>=kFrameSlots)
        throw std::invalid_argument("VulkanChunkStreamGpu::record invalid frame");
    // Invalidate even on a no-op record or a reused command-buffer handle.
    ++lightFrameSerial_;lightFrameCmd_=VK_NULL_HANDLE;lightFrameSlot_=UINT32_MAX;
    const auto finish=[&]{lightFrameCmd_=primary;lightFrameSlot_=frameSlot;};
    // beginFrame has waited this slot's fence; all earlier queue work and the
    // previous migration using these source buffers have completed.
    for(auto& old:retiredVoxels_[frameSlot])old.destroy();
    overlayLeases_[frameSlot].clear();
    if(readbacks_[frameSlot].snapshot)readbacks_[frameSlot].ready=true;
    for(auto& cooldown:retryFrames_)if(cooldown)--cooldown;
    std::array<uint32_t,3> planned{};
    for(size_t k=0;k<3;++k)planned[k]=pages_[k].densePages();
    VulkanChunkStreamPlan::Admission admit;
    if(config_.indirectVoxelPages && config_.growVoxelArenas){
        admit=[&](uint32_t slot,const LiveChunkPacket& packet,bool uploadVoxels){
            if(!uploadVoxels)return true;
            const std::array<bool,3> dense{packet.blockWords.size()!=1,packet.metaWords.size()!=1,packet.lightWords.size()!=1 || packet.gpuBlockLightOwned};
            auto next=planned;
            for(size_t k=0;k<3;++k){
                const auto& old=pages_[k].at(slot);
                // A committed inline transition frees its old page before
                // later uploads. All eviction pages were credited before admission.
                const bool wasDense=old.active && !old.uniform;
                if(packet.lightOnly && k<2)continue;
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
    StreamBatch batch;
    {
        std::lock_guard lock(ownershipMutex_);
        for(const auto& [key,e]:owned_)if(e.retiring)planner_->evict(key);
        batch=planner_->take(config_.uploadsPerFrame,admit,onRelease,!allowUploads);
        for(auto it=owned_.begin();it!=owned_.end();){
            if(it->second.retiring && !planner_->lookup(it->first) && !planner_->hasPendingChange(it->first))
                it=owned_.erase(it);
            else ++it;
        }
    }
    batch.streamOwner=this;batch.streamRecordSerial=lightFrameSerial_;
    tryShrinkChannels(frameSlot,batch.uploads.empty() && batch.releases.empty() && planner_->pendingCount()==0);
    const bool migrating=std::any_of(retiredVoxels_[frameSlot].begin(),retiredVoxels_[frameSlot].end(),
        [](const auto& b){return bool(b);});
    if (batch.empty() && dirtyGeometry_.empty() && !migrating){finish();return batch;}
    // Stage only the changed original voxel data, never the entire resident set.
    stagingBytes_.clear();
    Builder stage{stagingBytes_};
    RangeCopies regions;
    std::array<std::vector<Fill>,3> voxelFills;
    struct OverlayCopy {VkBuffer source; size_t kind; VkBufferCopy region;};
    std::vector<OverlayCopy> overlayCopies;
    auto stageDense=[&](size_t kind,VkDeviceSize dest,const std::vector<uint32_t>& words,
                        std::vector<VkBufferCopy>& copies,const std::shared_ptr<OverlaySnapshot>& source){
        if(source && kind<2){
            const auto size=VkDeviceSize(words.size())*4;
            overlayCopies.push_back({source->buffer.buffer(),kind,{kind==0?0:OverlaySnapshot::blockBytes,dest,size}});
            uploadStats_.overlayReusedBytes+=size;
        }else{
            stage.push(copies,dest,words.data(),words.size());
            uploadStats_.copiedBytes+=words.size()*4;
        }
    };
    auto channel=[&](uint32_t slot,size_t kind,VkDeviceSize bytes,
                     const std::vector<uint32_t>& words,std::vector<VkBufferCopy>& copies,bool forceDense=false,
                     const std::shared_ptr<OverlaySnapshot>& source={}){
        if(config_.indirectVoxelPages){
            auto& pool=pages_[kind];
            const auto old=pool.at(slot);
            const auto next=pool.set(slot,words.size()==1 && !forceDense,words[0]);
            if(next.uniform){
                if(old.active && old.uniform && old.value==next.value)uploadStats_.reusedBytes+=bytes;
                else uploadStats_.inlineBytes+=bytes;
            }else{
                if(words.size()==1){
                    voxelFills[kind].push_back({VkDeviceSize(next.value)*4,bytes,words[0]});
                    uploadStats_.filledBytes+=bytes;
                }else{
                    stageDense(kind,VkDeviceSize(next.value)*4,words,copies,source);
                }
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
            stageDense(kind,dest,words,copies,source);
            old={};
        }
    };
    std::vector<uint32_t> drawResets;
    drawResets.reserve(batch.releases.size()+batch.uploads.size());
    for (const auto& release:batch.releases) {
        drawResets.push_back(release.slot);
        opaqueFaces_[release.slot]={};miscFaces_[release.slot]={};
        repeated_[release.slot]={};
        skyRoofs_[release.slot]={};
        // Page ownership was already released by onRelease, before admission.
    }
    for (auto& upload:batch.uploads) {
        const auto slot=upload.slot;
        if(upload.resetDraw){
            repeated_[slot]={};
            skyRoofs_[slot]={};
            if(config_.indirectVoxelPages)for(auto& pool:pages_)pool.release(slot);
        }
        if(upload.uploadVoxels){
            dataSerials_[slot]=++nextDataSerial_;
            if(!upload.packet.lightOnly){
                skyRoofs_[slot]={};
                auto source=std::move(upload.packet.gpuSourceLease);
                if(source){
                    // Strong queued lease guarantees immutable bytes through
                    // admission; transfer the lease to the current frame fence.
                    overlayLeases_[frameSlot].push_back(source);
                    if(config_.indirectVoxelPages)skyRoofs_[slot]=upload.packet.blockWords.empty()?source->skyRoof:SkyRoofPalette::fromBlocks(upload.packet.blockWords);
                    for(size_t kind=0;kind<2;++kind){
                        const auto& words=kind==0?upload.packet.blockWords:upload.packet.metaWords;
                        if(!words.empty()){
                            channel(slot,kind,kChannelBytes[kind],words,kind==0?regions.blocks:regions.meta);
                            continue;
                        }
                        VkDeviceSize dest=VkDeviceSize(slot)*kChannelBytes[kind];
                        if(config_.indirectVoxelPages)
                            dest=VkDeviceSize(pages_[kind].set(slot,false,0).value)*4;
                        else repeated_[slot][kind]={};
                        overlayCopies.push_back({source->buffer.buffer(),kind,
                            {kind==0?0:OverlaySnapshot::blockBytes,dest,kChannelBytes[kind]}});
                        uploadStats_.overlayReusedBytes+=kChannelBytes[kind];
                    }
                }else{
                    source=acquireOverlay(upload.packet.overlayReference,device_);
                    if(source && (upload.packet.blockWords.size()!=1 || upload.packet.metaWords.size()!=1))
                        overlayLeases_[frameSlot].push_back(source);
                    // Certify the actual source, not unused CPU fallback words.
                    if(config_.indirectVoxelPages)
                        skyRoofs_[slot]=source && upload.packet.blockWords.size()!=1?source->skyRoof:SkyRoofPalette::fromBlocks(upload.packet.blockWords);
                    channel(slot,0,kBlockBytes,upload.packet.blockWords,regions.blocks,false,source);
                    channel(slot,1,kMetaBytes,upload.packet.metaWords,regions.meta,false,source);
                }
            }
            channel(slot,2,kLightBytes,upload.packet.lightWords,regions.light,upload.packet.gpuBlockLightOwned);
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
                         VK_ACCESS_INDIRECT_COMMAND_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT;
    before.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT|((migrating||!overlayCopies.empty())?VK_ACCESS_TRANSFER_READ_BIT:0u);
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
    for(const auto& c:overlayCopies)vkCmdCopyBuffer(primary,c.source,targets[c.kind],1,&c.region);
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
    finish();return batch;
}
void VulkanChunkStreamGpu::shutdown() noexcept {
    ++lightFrameSerial_;lightFrameCmd_=VK_NULL_HANDLE;lightFrameSlot_=UINT32_MAX;
    owned_.clear(); // shutdown requires detached producers and idle GPU
    planner_.reset();
    for(auto& frame:readbacks_){frame.snapshot.reset();frame.ready=false;frame.buffer.destroy();}
    dataSerials_.clear();
    for(auto& frame:overlayLeases_)frame.clear();
    for(auto& frame:retiredVoxels_)for(auto& b:frame)b.destroy();
    for(auto& frame:staging_)frame.buffer.destroy();
    liveSlots_.destroy();publishedDraws_.destroy();miscSlots_.destroy();
    opaqueSlots_.destroy();lightWords_.destroy();metaWords_.destroy();blockWords_.destroy();
    opaqueFaces_.clear();miscFaces_.clear();dirtyGeometry_.clear();stagingBytes_.clear();
    repeated_.clear();skyRoofs_.clear();for(auto& pool:pages_)pool.reset(0,1);uploadStats_={};
    liveSlotCount_=0;device_=VK_NULL_HANDLE;physical_=VK_NULL_HANDLE;
    maxPages_={};retryFrames_={};idleFrames_={};voxelEpoch_=growthCount_=shrinkCount_=admissionMisses_=0;
}
} // namespace OptiCraft::Vulkan
