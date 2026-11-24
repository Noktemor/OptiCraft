#include "rendering/vulkan/VulkanGameChunkFeed.h"
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
    for(size_t i=0;i<count;++i)words[i/4]|=uint32_t(src[i])<<((i%4)*8);
    return words;
}
}
void VulkanGameChunkFeed::setSink(UploadSink upload,EvictSink evict) {
    // A newly attached backend must receive fresh snapshots of everything.
    clear(); upload_=std::move(upload); evict_=std::move(evict);
}
void VulkanGameChunkFeed::acceptFrame(const std::vector<Renderable_Chunk>& rendered,
                                     const Chunk_Manager& world,int32_t dimension,size_t maxPackets) {
    if(!upload_ || !maxPackets || !trackedBudget_)return; // No idle GPU-related CPU work.
    // The OpenGL rendering distance may contain many thousands of chunks, but
    // this backend owns only a bounded number of GPU slots. Prioritize the
    // nearest resident chunks and evict those no longer inside that budget;
    // otherwise its pending queue would eventually overflow and abort Game.
    std::vector<const Renderable_Chunk*> selected;
    selected.reserve(rendered.size());
    for(const auto& chunk:rendered)selected.push_back(&chunk);
    auto distance=[this](const Renderable_Chunk* c) {
        const double dx=double(c->position.x)-centerChunk_[0];
        const double dy=double(c->position.y)-centerChunk_[1];
        const double dz=double(c->position.z)-centerChunk_[2];
        return dx*dx+dy*dy+dz*dz;
    };
    const auto nearer=[&](const Renderable_Chunk* a,const Renderable_Chunk* b) {
        const auto da=distance(a), db=distance(b);
        if(da!=db)return da<db;
        if(a->position.x!=b->position.x)return a->position.x<b->position.x;
        if(a->position.y!=b->position.y)return a->position.y<b->position.y;
        return a->position.z<b->position.z;
    };
    if(selected.size()>trackedBudget_) {
        std::nth_element(selected.begin(),selected.begin()+trackedBudget_,selected.end(),nearer);
        selected.resize(trackedBudget_);
    }
    std::sort(selected.begin(),selected.end(),nearer);
    std::unordered_set<ChunkKey,ChunkKeyHash> visible;
    visible.reserve(selected.size());
    for(const auto* c:selected)
        visible.insert({dimension,c->position.x,c->position.y,c->position.z});
    for(auto it=sent_.begin();it!=sent_.end();) {
        if(!visible.contains(it->first)) {
            if(evict_)evict_(it->first);
            it=sent_.erase(it);
        }else ++it;
    }
    std::vector<LiveChunkPacket> changes;
    // Do not serialize the whole world in one frame; the GPU uploader applies
    // its own independent per-frame budget after this bounded producer batch.
    changes.reserve(std::min<size_t>(selected.size(),maxPackets));
    std::array<uint16_t,kChunkVoxels> blocks;
    std::array<uint8_t,kChunkVoxels> metas, lights;
    for(const auto* candidate:selected){
        const Renderable_Chunk& entry=*candidate;
        const Chunk* chunk=world.get_chunk(entry.position.x,entry.position.y,entry.position.z);
        if(!chunk)continue;
        const ChunkKey key{dimension,entry.position.x,entry.position.y,entry.position.z};
        const uint64_t inst=chunk->get_instance_id();
        const uint64_t bv=chunk->get_blocks_version();
        const uint64_t mv=chunk->get_dirty_epoch();
        auto found=sent_.find(key);
        if(found!=sent_.end() && found->second.instance==inst &&
           found->second.blocks==bv && found->second.mesh==mv)continue;
        LiveChunkPacket pkt{};
        pkt.position=key;pkt.chunkInstance=inst;pkt.blocksVersion=bv;pkt.meshVersion=mv;
        chunk->get_blocks().unpack(blocks.data());
        pkt.blockWords.resize(kChunkBlockWords);
        packBlockIds(blocks,pkt.blockWords);
        chunk->get_blocks().unpack_meta(metas.data());
        pkt.metaWords=packBytes(metas.data(),metas.size());
        chunk->get_light_grid().unpack(lights.data());
        pkt.lightWords=packBytes(lights.data(),lights.size());
        changes.push_back(std::move(pkt));
        // The sink's packets must be accepted before this revision is marked sent.
        sent_[key]={inst,bv,mv};
        if(changes.size()>=maxPackets)break;
    }
    if(!changes.empty()) {
        try {upload_(std::move(changes));}
        catch(...) {sent_.clear();throw;} // retry a complete snapshot after consumer failure
    }
}
void VulkanGameChunkFeed::chunkUnloaded(ChunkKey key){
    if(sent_.erase(key) && evict_)evict_(key);
}
void VulkanGameChunkFeed::clear(){
    if(evict_)for(const auto& pair:sent_)evict_(pair.first);
    sent_.clear();
}
} // namespace OptiCraft::Vulkan
