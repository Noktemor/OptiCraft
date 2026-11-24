// OptiCraft Vulkan mesh ABI v1, shared between compute and vertex shaders.
// 32^3 chunks. GPU block storage: two unsigned 16-bit block IDs per uint32.
// Stable chunk slots are NOT positions in the current dispatch batch.
struct ChunkSlot {
    uvec4 source;        // x=block base (uint words), y=face base (uvec2), z=face capacity, w=flags
    ivec4 origin;        // world voxel-space origin; w reserved
    ivec4 neighborA;     // block-word offsets -X,+X,-Y,+Y; -1 means absent/air
    ivec4 neighborB;     // -Z,+Z,unused,unused
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
layout(push_constant) uniform DispatchParams { uint firstJob; uint totalJobs; } dispatchParams;

const uint CHUNK_SIZE = 32u;
const uint CHUNK_VOXELS = 32768u;
const uint BLOCK_CUBE = 1u;
const uint BLOCK_OCCLUDER = 2u;

uint atIndex(uint wordBase, uint index) {
    uint packed = blocks[wordBase + (index >> 1u)];
    return ((index & 1u) != 0u) ? packed >> 16u : packed & 65535u;
}
uint atLocal(int wordBase, ivec3 p) {
    if (wordBase < 0) return 0u;
    uint index = uint(p.y * 1024 + p.z * 32 + p.x);
    return atIndex(uint(wordBase), index);
}
uint cellAt(ChunkSlot chunk, ivec3 p) {
    if (p.x < 0) return atLocal(chunk.neighborA.x, ivec3(31, p.y, p.z));
    if (p.x >= 32) return atLocal(chunk.neighborA.y, ivec3(0, p.y, p.z));
    if (p.y < 0) return atLocal(chunk.neighborA.z, ivec3(p.x, 31, p.z));
    if (p.y >= 32) return atLocal(chunk.neighborA.w, ivec3(p.x, 0, p.z));
    if (p.z < 0) return atLocal(chunk.neighborB.x, ivec3(p.x, p.y, 31));
    if (p.z >= 32) return atLocal(chunk.neighborB.y, ivec3(p.x, p.y, 0));
    return atLocal(int(chunk.source.x), p);
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
