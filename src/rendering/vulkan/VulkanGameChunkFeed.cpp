#include "rendering/vulkan/VulkanGameChunkFeed.h"
#include "rendering/vulkan/MeshResidency.h"
#include "rendering/vulkan/SurfaceLod.h"
#include <cstdlib>
#include <string_view>
#include "world/chunks/Chunk_Manager.h"
#include "world/chunks/Chunk.h"
#include "utils/configs/Config_World.h"
#include <algorithm>
#include <unordered_set>
#include <array>
#include <utility>

namespace OptiCraft::Vulkan {
namespace {
std::vector<uint32_t> packBytes(const uint8_t* src,size_t count) {
    std::vector<uint32_t> words((count+3)/4);
    size_t i=0;
    for(;i+4<=count;i+=4)words[i/4]=uint32_t(src[i])|(uint32_t(src[i+1])<<8u)|(uint32_t(src[i+2])<<16u)|(uint32_t(src[i+3])<<24u);
    for(;i<count;++i)words[i/4]|=uint32_t(src[i])<<((i%4)*8);
    return words;
}
}
void VulkanGameChunkFeed::setSink(UploadSink upload,EvictSink evict,SnapshotLeaseSink lease) {
    // A newly attached backend must receive fresh snapshots of everything.
    clear(); upload_=std::move(upload); evict_=std::move(evict); lease_=std::move(lease);
}
void VulkanGameChunkFeed::acceptFrame(const std::vector<Renderable_Chunk>& rendered,
                                     const Chunk_Manager& world,int32_t dimension,size_t maxPackets) {
    if(!upload_ || !trackedBudget_)return; // No idle GPU-related CPU work.
    // The OpenGL rendering distance may contain many thousands of chunks, but
    // this backend owns only a bounded number of GPU slots. Prioritize the
    // nearest resident chunks and evict those no longer inside that budget;
    // otherwise its pending queue would eventually overflow and abort Game.
    const auto now=std::chrono::steady_clock::now();
    static const bool compactUpload=[](){const char* s=std::getenv("OPTICRAFT_VULKAN_COMPACT_UPLOAD");return !s||std::string_view(s)!="0";}();
    static const bool lodEnabled=[](){const char* s=std::getenv("OPTICRAFT_VULKAN_LOD");return !s||std::string_view(s)!="0";}();
    static const bool geometryEnabled=[](){const char* s=std::getenv("OPTICRAFT_VULKAN_GEOMETRY_LOD");return !s||std::string_view(s)!="0";}();
    static const bool compactLod=[](){const char* s=std::getenv("OPTICRAFT_VULKAN_LOD_CONTROL");return !s||std::string_view(s)!="0";}();
    static const bool compactLight=[](){const char* s=std::getenv("OPTICRAFT_VULKAN_LIGHT_CONTROL");return !s||std::string_view(s)!="0";}();
    static const bool gpuSourcePackets=[](){const char* s=std::getenv("OPTICRAFT_VULKAN_GPU_SOURCE_PACKETS");return !s||std::string_view(s)!="0";}();
    auto distance=[this](const ChunkKey& c) {
        const double dx=double(c.x)-centerChunk_[0];
        const double dy=double(c.y)-centerChunk_[1];
        const double dz=double(c.z)-centerChunk_[2];
        return dx*dx+dy*dy+dz*dz;
    };
    const auto epoch=world.vulkan_resident_epoch();
    const auto dirtyEpoch=Chunk::dirty_event_count();
    const auto interactiveEpoch=Chunk::interactive_event_count();
    const bool newInteractive=seenInteractiveEpoch_!=interactiveEpoch;
    const bool scanInteractive=drainedInteractiveEpoch_!=interactiveEpoch;
    const bool moved=selectionCenter_!=centerChunk_ || selectionDimension_!=dimension || selectionBudget_!=trackedBudget_;
    const bool reselect=newInteractive || moved || selectionEpoch_==UINT64_MAX || (selectionEpoch_!=epoch &&
           now-lastSelection_>=std::chrono::milliseconds(16)) ||
           (selectionDirtyEpoch_!=dirtyEpoch && now-lastSelection_>=std::chrono::milliseconds(100));
    if(reselect){
        std::vector<ChunkKey> selected;
        selected.reserve(rendered.size());
        std::unordered_set<ChunkKey,ChunkKeyHash> nonempty;
        nonempty.reserve(rendered.size());
        for(const auto& c:rendered){
            const Chunk* chunk=world.get_chunk(c.position.x,c.position.y,c.position.z);
            if(!chunk)continue;
            const ChunkKey key{dimension,c.position.x,c.position.y,c.position.z};
            selected.push_back(key);
            if(chunk->has_blocks())nonempty.insert(key);
        }
        // Remote air has no mesh and is never sampled by a rendered neighbour.
        // Air adjacent to terrain is retained, including its exact light grid.
        remoteAirSkipped_=std::erase_if(selected,[&](const ChunkKey& key){
            return !nonempty.contains(key) && !needsAirHalo(key,nonempty);
        });
        const auto nearer=[&](const ChunkKey& a,const ChunkKey& b){
            const auto da=distance(a),db=distance(b);
            if(da!=db)return da<db;
            if(a.x!=b.x)return a.x<b.x;
            if(a.y!=b.y)return a.y<b.y;
            return a.z<b.z;
        };
        if(selected.size()>trackedBudget_){
            std::nth_element(selected.begin(),selected.begin()+trackedBudget_,selected.end(),nearer);
            selected.resize(trackedBudget_);
        }
        std::sort(selected.begin(),selected.end(),nearer);
        std::unordered_set<ChunkKey,ChunkKeyHash> visible(selected.begin(),selected.end());
        // One conservative render-only pilot core. Keep CPU snapshots complete;
        // do not request extra resident slots or force uniform pages dense.
        auto eligible=[&](const ChunkKey& k,int radius){
            if(!visible.contains(k) || !nonempty.contains(k))return false;
            const auto dx=std::abs(int64_t(k.x)-centerChunk_[0]);
            const auto dz=std::abs(int64_t(k.z)-centerChunk_[2]);
            if(std::max(dx,dz)<radius)return false;
            const auto* c=world.get_chunk(k.x,k.y,k.z);
            if(!c || c->get_light_grid().is_uniform())return false;
            for(int x=-1;x<=1;++x)for(int y=-1;y<=1;++y)for(int z=-1;z<=1;++z){
                const int64_t nx=int64_t(k.x)+x,ny=int64_t(k.y)+y,nz=int64_t(k.z)+z;
                if(nx<INT32_MIN||nx>INT32_MAX||ny<INT32_MIN||ny>INT32_MAX||nz<INT32_MIN||nz>INT32_MAX ||
                   !visible.contains({k.dimension,int32_t(nx),int32_t(ny),int32_t(nz)}))return false;
            }
            return true;
        };
        if(!residentLightPilot_)lightCore_.reset();
        else if(!lightCore_ || !eligible(*lightCore_,6)){
            lightCore_.reset();
            for(const auto& k:selected)if(eligible(k,8)){lightCore_=k;break;}
        }
        // Hand back the previous owner before a new promotion or ordinary
        // snapshots consume the bounded packet budget.
        std::stable_partition(selected.begin(),selected.end(),[&](const ChunkKey& k){
            const auto old=sent_.find(k);
            return old!=sent_.end() && old->second.gpuLight && (!lightCore_ || k!=*lightCore_);
        });
        for(auto it=sent_.begin();it!=sent_.end();){
            if(!visible.contains(it->first)){
                if(evict_)evict_(it->first);
                it=sent_.erase(it);
            }else ++it;
        }
        selectedKeys_=std::move(selected);
        selectionEpoch_=epoch;selectionCenter_=centerChunk_;selectionDimension_=dimension;
        selectionBudget_=trackedBudget_;lastSelection_=now;selectionDirtyEpoch_=dirtyEpoch;
        snapshotDirtyEpoch_=UINT64_MAX;
        seenInteractiveEpoch_=interactiveEpoch;
    }
    if(!maxPackets && !scanInteractive)return; // still evict obsolete slots during backpressure
    // Do not rescan thousands of chunk revisions at 1000+ presentation FPS.
    // Snapshot cadence is independent of presentation; new camera chunks bypass it.
    if(!scanInteractive && !reselect && selectionEpoch_==epoch && snapshotDirtyEpoch_==dirtyEpoch &&
       sent_.size()==selectedKeys_.size() && now-lastCompleteScan_<std::chrono::milliseconds(250))return;
    if(!scanInteractive && !reselect && !moved && now-lastSnapshot_<std::chrono::milliseconds(4))return;
    lastSnapshot_=now;
    std::vector<LiveChunkPacket> changes;
    // Do not serialize the whole world in one frame; the GPU uploader applies
    // its own independent per-frame budget after this bounded producer batch.
    constexpr size_t interactiveCap=8;
    changes.reserve(std::min<size_t>(selectedKeys_.size(),std::max(maxPackets,interactiveCap)));
    auto isInteractive=[&](const ChunkKey& key,const Chunk* chunk){
        if(!chunk || !chunk->interactive_mesh_epoch())return false;
        const auto it=sent_.find(key);
        return it==sent_.end() || it->second.instance!=chunk->get_instance_id() ||
               it->second.interactiveEpoch!=chunk->interactive_mesh_epoch();
    };
    std::vector<ChunkKey> editKeys;
    if(scanInteractive || reselect)for(const auto& key:selectedKeys_)
        if(isInteractive(key,world.get_chunk(key.x,key.y,key.z)))editKeys.push_back(key);
    // New edit packets bypass the normal producer cadence and backpressure.
    std::vector<ChunkKey> ordered;
    if(!editKeys.empty()){
        ordered=editKeys;
        std::unordered_set<ChunkKey,ChunkKeyHash> edits(editKeys.begin(),editKeys.end());
        for(const auto& key:selectedKeys_)if(!edits.contains(key))ordered.push_back(key);
    }
    const auto& keys=ordered.empty()?selectedKeys_:ordered;
    const size_t packetCap=editKeys.empty()?maxPackets:std::max(maxPackets,std::min(interactiveCap,editKeys.size()));
    size_t sentEdits=0;
    std::array<uint16_t,kChunkVoxels> blocks;
    std::array<uint8_t,kChunkVoxels> metas, lights;
    for(const auto& key:keys){
        const Chunk* chunk=world.get_chunk(key.x,key.y,key.z);
        if(!chunk)continue;
        const bool interactive=isInteractive(key,chunk);
        if(changes.size()>=packetCap || (!interactive && changes.size()>=maxPackets))break;
        const uint64_t inst=chunk->get_instance_id();
        const uint64_t bv=chunk->get_blocks_version();
        const uint64_t mv=chunk->get_dirty_epoch();
        auto found=sent_.find(key);
        const bool sameInstance=found!=sent_.end() && !found->second.needsFull && found->second.instance==inst;
        const uint32_t wasLod=sameInstance?found->second.lod:0u;
        const double dist=distance(key);
        const bool gpuLight=residentLightPilot_ && lightCore_ && key==*lightCore_;
        if(!interactive && sameInstance &&
           found->second.blocks==bv && found->second.mesh==mv && found->second.gpuLight==gpuLight &&
           found->second.lod==selectSurfaceLod(dist,wasLod,found->second.opaque,found->second.geometryEligible,lodEnabled,geometryEnabled))continue;
        LiveChunkPacket pkt{};
        pkt.position=key;pkt.chunkInstance=inst;pkt.blocksVersion=bv;pkt.meshVersion=mv;
        pkt.priority=static_cast<uint64_t>(distance(key));
        pkt.interactive=interactive;
        pkt.overlayReference=chunk->overlay_reference();
        if(hasKinds_){
            pkt.meshKinds=0;
            bool allOpaque=true;
            for(const auto id:chunk->get_blocks().palette()){
                const auto kind=meshKinds_[static_cast<uint16_t>(id)];
                pkt.meshKinds|=kind;
                allOpaque=allOpaque && kind==1u;
            }
            // Palette can include unused entries: that only disables an optimization.
            // Air and non-occluding materials can never set this bit.
            if(allOpaque && pkt.meshKinds==1u)pkt.meshKinds|=4u;
            if(!chunk->has_blocks())pkt.meshKinds=0u;
        }
        const bool opaque=(pkt.meshKinds&1u)!=0;
        // Only a palette-proven opaque/air chunk may approximate its interior.
        const auto& palette=chunk->get_blocks().palette();
        const bool geometryEligible=hasKinds_ && opaque && (pkt.meshKinds&6u)==0u &&
            std::all_of(palette.begin(),palette.end(),
                [&](auto id){return static_cast<uint16_t>(id)==0u || meshKinds_[static_cast<uint16_t>(id)]==1u;});
        const uint32_t lod=selectSurfaceLod(dist,wasLod,opaque,geometryEligible,lodEnabled,geometryEnabled);
        pkt.meshKinds|=lod;
        pkt.lodRevision=++nextLodRevision_;
        pkt.gpuBlockLightOwned=gpuLight;
        pkt.lightOwnershipRevision=sameInstance && found->second.gpuLight==gpuLight
            ?found->second.lightOwnershipRevision:pkt.lodRevision;
        // The same producer revision already has a full snapshot queued or
        // resident. Camera-only changes need neither unpacking nor a snapshot lease.
        if(compactLod && sameInstance && found->second.blocks==bv && found->second.mesh==mv &&
           found->second.gpuLight==gpuLight && found->second.opaque==opaque &&
           found->second.geometryEligible==geometryEligible && found->second.lod!=lod){
            pkt.lodOnly=true;
            pkt.overlayReference={};
        } else {
            const auto& grid=chunk->get_blocks();
            const auto& lightGrid=chunk->get_light_grid();
            pkt.lightOnly=compactLight && sameInstance && found->second.blocks==bv &&
                (found->second.mesh!=mv || found->second.gpuLight!=gpuLight) &&
                found->second.opaque==opaque && found->second.geometryEligible==geometryEligible;
            if(pkt.lightOnly) pkt.overlayReference={};
            else {
                // A strong lease is acquired on the graphics owner thread
                // BEFORE omitting any payload. Ordinary CPU packets remain
                // the fallback if the bounded snapshot pool has expired.
                if(gpuSourcePackets && lease_ && pkt.overlayReference.ticket && (!compactUpload || !grid.is_uniform()))
                    pkt.gpuSourceLease=lease_(pkt.overlayReference);
                if(compactUpload && grid.is_uniform()) {
                    const auto id=uint32_t(static_cast<uint16_t>(grid.uniform_block()));
                    pkt.blockWords={id|(id<<16u)};
                } else if(!pkt.gpuSourceLease) {
                    grid.unpack(blocks.data());
                    pkt.blockWords.resize(kChunkBlockWords);
                    packBlockIds(blocks,pkt.blockWords);
                }
                if(compactUpload) {
                    pkt.metaWords=packSparseMetadata(grid.meta_entries());
                    // Preserve the existing uniform-channel optimization.
                    if(pkt.gpuSourceLease && pkt.metaWords.size()!=1)
                        std::vector<uint32_t>{}.swap(pkt.metaWords);
                } else if(!pkt.gpuSourceLease) {
                    grid.unpack_meta(metas.data());
                    pkt.metaWords=packBytes(metas.data(),metas.size());
                }
            }
            if(compactUpload && lightGrid.is_uniform())
                pkt.lightWords={uint32_t(lightGrid.get(0,0,0))*0x01010101u};
            else {
                lightGrid.unpack(lights.data());
                pkt.lightWords=packBytes(lights.data(),lights.size());
            }
        }
        changes.push_back(std::move(pkt));
        // The sink's packets must be accepted before this revision is marked sent.
        sent_[key]={inst,bv,mv,lod,opaque,geometryEligible,gpuLight,changes.back().lightOwnershipRevision};
        sent_[key].interactiveEpoch=chunk->interactive_mesh_epoch();
        if(interactive)++sentEdits;
    }
    if(sentEdits==editKeys.size())drainedInteractiveEpoch_=interactiveEpoch;
    if(changes.size()<maxPackets){snapshotDirtyEpoch_=dirtyEpoch;lastCompleteScan_=now;}
    if(!changes.empty()) {
        try {upload_(std::move(changes));}
        catch(const LodControlSourceMissing&) {
            // The planner preflight rejected the entire callback; no partial
            // acknowledgement. Retain the selection but retry full snapshots.
            for(auto& [key,version]:sent_){version.needsFull=true;version.interactiveEpoch=0;}
            drainedInteractiveEpoch_=0;
            snapshotDirtyEpoch_=UINT64_MAX;lastSnapshot_={};
        }
        catch(...) {
            for(auto& [key,version]:sent_){version.needsFull=true;version.interactiveEpoch=0;}
            drainedInteractiveEpoch_=0;
            snapshotDirtyEpoch_=UINT64_MAX;lastSnapshot_={};throw;
        } // retry a complete snapshot after consumer failure
    }
}
void VulkanGameChunkFeed::chunkUnloaded(ChunkKey key){
    if(sent_.erase(key) && evict_)evict_(key);
}
void VulkanGameChunkFeed::clear(){
    if(evict_)for(const auto& pair:sent_)evict_(pair.first);
    sent_.clear();selectedKeys_.clear();lightCore_.reset();selectionEpoch_=UINT64_MAX;remoteAirSkipped_=0;
    seenInteractiveEpoch_=drainedInteractiveEpoch_=0;
    lastSnapshot_={};lastSelection_={};lastCompleteScan_={};selectionDirtyEpoch_=snapshotDirtyEpoch_=UINT64_MAX;
}
} // namespace OptiCraft::Vulkan
