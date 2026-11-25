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
void VulkanGameChunkFeed::setSink(UploadSink upload,EvictSink evict) {
    // A newly attached backend must receive fresh snapshots of everything.
    clear(); upload_=std::move(upload); evict_=std::move(evict);
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
    auto distance=[this](const ChunkKey& c) {
        const double dx=double(c.x)-centerChunk_[0];
        const double dy=double(c.y)-centerChunk_[1];
        const double dz=double(c.z)-centerChunk_[2];
        return dx*dx+dy*dy+dz*dz;
    };
    const auto epoch=world.vulkan_resident_epoch();
    const auto dirtyEpoch=Chunk::dirty_event_count();
    const bool moved=selectionCenter_!=centerChunk_ || selectionDimension_!=dimension || selectionBudget_!=trackedBudget_;
    const bool reselect=moved || selectionEpoch_==UINT64_MAX || (selectionEpoch_!=epoch &&
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
    }
    if(!maxPackets)return; // still evict obsolete slots during backpressure
    // Do not rescan thousands of chunk revisions at 1000+ presentation FPS.
    // Snapshot cadence is independent of presentation; new camera chunks bypass it.
    if(!reselect && selectionEpoch_==epoch && snapshotDirtyEpoch_==dirtyEpoch &&
       sent_.size()==selectedKeys_.size() && now-lastCompleteScan_<std::chrono::milliseconds(250))return;
    if(!moved && now-lastSnapshot_<std::chrono::milliseconds(4))return;
    lastSnapshot_=now;
    std::vector<LiveChunkPacket> changes;
    // Do not serialize the whole world in one frame; the GPU uploader applies
    // its own independent per-frame budget after this bounded producer batch.
    changes.reserve(std::min<size_t>(selectedKeys_.size(),maxPackets));
    std::array<uint16_t,kChunkVoxels> blocks;
    std::array<uint8_t,kChunkVoxels> metas, lights;
    for(const auto& key:selectedKeys_){
        const Chunk* chunk=world.get_chunk(key.x,key.y,key.z);
        if(!chunk)continue;
        const uint64_t inst=chunk->get_instance_id();
        const uint64_t bv=chunk->get_blocks_version();
        const uint64_t mv=chunk->get_dirty_epoch();
        auto found=sent_.find(key);
        const bool sameInstance=found!=sent_.end() && found->second.instance==inst;
        const uint32_t wasLod=sameInstance?found->second.lod:0u;
        const double dist=distance(key);
        if(found!=sent_.end() && found->second.instance==inst &&
           found->second.blocks==bv && found->second.mesh==mv &&
           found->second.lod==selectSurfaceLod(dist,wasLod,found->second.opaque,found->second.geometryEligible,lodEnabled,geometryEnabled))continue;
        LiveChunkPacket pkt{};
        pkt.position=key;pkt.chunkInstance=inst;pkt.blocksVersion=bv;pkt.meshVersion=mv;
        pkt.priority=static_cast<uint64_t>(distance(key));
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
        const auto& grid=chunk->get_blocks();
        const auto& lightGrid=chunk->get_light_grid();
        if(compactUpload && grid.is_uniform()) {
            const auto id=uint32_t(static_cast<uint16_t>(grid.uniform_block()));
            pkt.blockWords={id|(id<<16u)};
        } else {
            grid.unpack(blocks.data());
            pkt.blockWords.resize(kChunkBlockWords);
            packBlockIds(blocks,pkt.blockWords);
        }
        if(compactUpload) pkt.metaWords=packSparseMetadata(grid.meta_entries());
        else {
            grid.unpack_meta(metas.data());
            pkt.metaWords=packBytes(metas.data(),metas.size());
        }
        if(compactUpload && lightGrid.is_uniform())
            pkt.lightWords={uint32_t(lightGrid.get(0,0,0))*0x01010101u};
        else {
            lightGrid.unpack(lights.data());
            pkt.lightWords=packBytes(lights.data(),lights.size());
        }
        changes.push_back(std::move(pkt));
        // The sink's packets must be accepted before this revision is marked sent.
        sent_[key]={inst,bv,mv,lod,opaque,geometryEligible};
        if(changes.size()>=maxPackets)break;
    }
    if(changes.size()<maxPackets){snapshotDirtyEpoch_=dirtyEpoch;lastCompleteScan_=now;}
    if(!changes.empty()) {
        try {upload_(std::move(changes));}
        catch(...) {sent_.clear();snapshotDirtyEpoch_=UINT64_MAX;throw;} // retry a complete snapshot after consumer failure
    }
}
void VulkanGameChunkFeed::chunkUnloaded(ChunkKey key){
    if(sent_.erase(key) && evict_)evict_(key);
}
void VulkanGameChunkFeed::clear(){
    if(evict_)for(const auto& pair:sent_)evict_(pair.first);
    sent_.clear();selectedKeys_.clear();selectionEpoch_=UINT64_MAX;remoteAirSkipped_=0;
    lastSnapshot_={};lastSelection_={};lastCompleteScan_={};selectionDirtyEpoch_=snapshotDirtyEpoch_=UINT64_MAX;
}
} // namespace OptiCraft::Vulkan
