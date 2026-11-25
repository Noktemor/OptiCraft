#include "voxel_address.glsl"
// OptiCraft Vulkan mesh ABI, shared between compute and vertex shaders.
// 32^3 chunks. GPU block storage: two unsigned 16-bit block IDs per uint32.
// Stable chunk slots are NOT positions in the current dispatch batch.
struct ChunkSlot {
    uvec4 source;        // x=block base/inline word, y=face base (uvec2), z=face capacity, w=flags
    ivec4 origin;        // world voxel-space origin; w reserved
    ivec4 neighborA;     // neighbor IDs (legacy word offsets) -X,+X,-Y,+Y; -1 absent
    ivec4 neighborB;     // -Z,+Z,meta base/word,light base/word
};
layout(std430, set=0, binding=0) readonly buffer Voxels { uint blocks[]; };
layout(std430, set=0, binding=1) readonly buffer ChunkTable { ChunkSlot slots[]; };
layout(std430, set=0, binding=2) buffer ScratchCounts { uint counts[]; };
layout(std430, set=0, binding=3) buffer PackedFaces { uvec2 faces[]; };
layout(std430, set=0, binding=4) readonly buffer MaterialFlags { uint materialFlags[]; };
layout(std430, set=0, binding=5) buffer DrawCommands { uvec4 draws[]; };
layout(std430, set=0, binding=6) readonly buffer DispatchIndices { uint slotIndices[]; };
layout(std430, set=0, binding=7) buffer CandidateDrawCommands { uvec4 candidateDraws[]; };
layout(std430, set=0, binding=8) readonly buffer MetaWords {uint metaWords[];};
layout(std430, set=0, binding=9) readonly buffer LightWords {uint lightWords[];};
// Batch-private scratch: totalJobs uint count headers, followed by fixed
// per-job packed rectangle segments. Headers are copied from Count before reset.
layout(std430, set=0, binding=10) buffer RectangleCache { uint cachedWords[]; };
layout(push_constant) uniform DispatchParams { uint firstJob; uint totalJobs; uint cacheCapacity; uint fastBasePlusOne; } dispatchParams;
// High bit is only passed to the plane dispatches, not finalize/publish or fast copy.
const uint COARSE_CACHE_FLAG=0x80000000u;
const uint PLANE_COMPACT_FLAG=0x40000000u;
uint batchJobCount(){return dispatchParams.totalJobs&~(COARSE_CACHE_FLAG|PLANE_COMPACT_FLAG);}
bool usePlaneCompaction(){return (dispatchParams.totalJobs&PLANE_COMPACT_FLAG)!=0u;}
bool useCoarseCache(){return (dispatchParams.totalJobs&COARSE_CACHE_FLAG)!=0u;}
uint cacheWord(uint job,uint rect) {
    return batchJobCount()+2u*(job*dispatchParams.cacheCapacity+rect);
}
uint coarseWord(uint job,uint cell){
    return batchJobCount()*(1u+2u*dispatchParams.cacheCapacity)+job*4096u+cell;
}

// Suffix after the coarse cache: indirect arguments (3 words per job),
// followed by a fixed list of 192 plane IDs and 192 x 32 row masks per job.
uint planeArgsWord(uint job){return batchJobCount()*(1u+2u*dispatchParams.cacheCapacity+4096u)+3u*job;}
uint planeListWord(uint job,uint index){return planeArgsWord(batchJobCount())+job*6336u+index;}
uint planeRowWord(uint job,uint task,uint row){return planeListWord(job,192u+task*32u+row);}

const uint CHUNK_SIZE = 32u;
const uint CHUNK_VOXELS = 32768u;
const uint BLOCK_CUBE = 1u;
const uint BLOCK_OCCLUDER = 2u;

uint atIndex(uint wordBase, uint index, int flags) {
    uint packedValue=VOXEL_WORD(blocks,wordBase,index>>1u,flags,VOXEL_UNIFORM_BLOCK);
    return ((index & 1u) != 0u) ? packedValue >> 16u : packedValue & 65535u;
}
uint atLocal(ChunkSlot chunk,ivec3 p) {
    uint index=uint(p.y*1024+p.z*32+p.x);
    return atIndex(chunk.source.x,index,chunk.origin.w);
}
uint atNeighbor(ChunkSlot chunk,int link,ivec3 p){
    if(link<0)return 0u;
    if(voxelIndirect(chunk.origin.w))return atLocal(slots[uint(link)],p);
    return atIndex(uint(link),uint(p.y*1024+p.z*32+p.x),0);
}
uint cellAt(ChunkSlot chunk, ivec3 p) {
    if (p.x < 0) return atNeighbor(chunk,chunk.neighborA.x, ivec3(31, p.y, p.z));
    if (p.x >= 32) return atNeighbor(chunk,chunk.neighborA.y, ivec3(0, p.y, p.z));
    if (p.y < 0) return atNeighbor(chunk,chunk.neighborA.z, ivec3(p.x, 31, p.z));
    if (p.y >= 32) return atNeighbor(chunk,chunk.neighborA.w, ivec3(p.x, 0, p.z));
    if (p.z < 0) return atNeighbor(chunk,chunk.neighborB.x, ivec3(p.x, p.y, 31));
    if (p.z >= 32) return atNeighbor(chunk,chunk.neighborB.y, ivec3(p.x, p.y, 0));
    return atLocal(chunk,p);
}
ivec3 positionAt(uint idx) {
    return ivec3(int(idx & 31u), int(idx >> 10u), int((idx >> 5u) & 31u));
}
ivec3 direction(uint face) {
    if (face == 0u) return ivec3(-1, 0, 0);
    if (face == 1u) return ivec3( 1, 0, 0);
    if (face == 2u) return ivec3(0, -1, 0);
    if (face == 3u) return ivec3(0,  1, 0);
    if (face == 4u) return ivec3(0, 0, -1);
    return ivec3(0, 0, 1);
}
bool shouldEmit(ChunkSlot chunk, uint blockId, ivec3 p, uint face) {
    if ((materialFlags[blockId] & BLOCK_CUBE) == 0u) return false;
    uint neighborId = cellAt(chunk, p + direction(face));
    return (materialFlags[neighborId] & BLOCK_OCCLUDER) == 0u;
}
// Palette fast path: interior planes of a fully occluding chunk cannot emit.
// A face on the edge cannot emit either when its resident neighbour is all opaque.
bool skipOpaquePlane(ChunkSlot chunk,uint face,uint slice) {
    if((uint(chunk.origin.w)&8u)==0u)return false; // legacy/probe ABI: unknown palette
    if((uint(chunk.origin.w)&1u)==0u)return true;
    if((uint(chunk.origin.w)&4u)==0u)return false;
    if(slice!=((face&1u)==0u?0u:31u))return true;
    int neighbor=face<4u?chunk.neighborA[face]:chunk.neighborB[face-4u];
    if(neighbor<0)return false;
    return (uint(slots[voxelNeighborSlot(chunk.origin.w,neighbor)].origin.w)&12u)==12u;
}
// 8 bytes / visible cube face. .x: x[0..4] y[5..9] z[10..14]
// face[15..17] material-low[18..31]. .y: material-high[0..1] slot[2..17].
// .y: blockId bits 14..15, slot bits 2..17, (runLength-1) bits 18..22, (runHeight-1) bits 23..27.
// Stable slots must fit in 16 bits (enforced by VulkanChunkMeshing).
uvec2 packFace(ivec3 p, uint face, uint blockId, uint slot, uint runLength, uint runHeight) {
    uint word0 = uint(p.x) | (uint(p.y) << 5u) | (uint(p.z) << 10u) |
        (face << 15u) | ((blockId & 16383u) << 18u);
    uint word1 = (blockId >> 14u) | (slot << 2u) | ((runLength - 1u) << 18u) | ((runHeight - 1u) << 23u);
    return uvec2(word0, word1);
}
