#version 450
#extension GL_GOOGLE_include_directive : require
#include "voxel_address.glsl"
// GPU-packedValue opaque faces, with real per-block UVs and orientation metadata.
// No expanded vertices or indices exist on CPU or in GPU memory.
struct ChunkSlot {uvec4 source; ivec4 origin; ivec4 xa; ivec4 zb;};
struct Material {uint faceBase; uint metaStride; uint shape; uint flags;};
struct FaceUv {vec4 uv; uvec4 options;};
layout(std430,set=0,binding=0) readonly buffer ChunkSlots {ChunkSlot slots[];};
layout(std430,set=0,binding=1) readonly buffer Geometry {uvec2 faces[];};
layout(std430,set=0,binding=2) readonly buffer MaterialTable {Material materials[];};
layout(std430,set=0,binding=3) readonly buffer FaceTable {FaceUv faceUvs[];};
layout(std430,set=0,binding=4) readonly buffer MetaBytes {uint metadata[];};
layout(std430,set=0,binding=5) readonly buffer LightBytes {uint lightWords[];};
layout(push_constant) uniform View {mat4 vp; vec4 eyeAmbient; vec4 fogColorStart; vec4 fogEnd; vec4 heldLight;} camera;
layout(location=0) out vec2 vTileUv;
layout(location=2) out vec4 vAtlasRect;
layout(location=3) flat out uint vPackedLight;
layout(location=1) out vec3 vNormal;
const vec2 corners[6]=vec2[6](vec2(0,0),vec2(1,0),vec2(1,1),
                              vec2(0,0),vec2(1,1),vec2(0,1));
layout(location=4) out vec3 worldPosition;
layout(location=5) out vec3 localPosition;
layout(location=6) flat out uint chunkSlot;
layout(location=7) flat out uint surfaceLod;
void main(){
    uvec2 packedValue=faces[gl_InstanceIndex];
    uint face=(packedValue.x>>15u)&7u;
    surfaceLod=(packedValue.y>>28u)&1u;
    uint id=((packedValue.x>>18u)&16383u)|((packedValue.y&3u)<<14u);
    uint slot=(packedValue.y>>2u)&65535u;
    float runLength=float(((packedValue.y>>18u)&31u)+1u);
    float runHeight=float(((packedValue.y>>23u)&31u)+1u);
    uint x=packedValue.x&31u, y=(packedValue.x>>5u)&31u, z=(packedValue.x>>10u)&31u;
    vec3 p=vec3(x,y,z);
    vec2 uv=corners[gl_VertexIndex];
    if(face==0u || face==2u || face==5u)uv*=vec2(runLength,runHeight);
    else uv*=vec2(runHeight,runLength);
    vec3 offset;
    if(face==0u){offset=vec3(0,uv.y,uv.x);vNormal=vec3(-1,0,0);}
    else if(face==1u){offset=vec3(1,uv.x,uv.y);vNormal=vec3(1,0,0);}
    else if(face==2u){offset=vec3(uv.x,0,uv.y);vNormal=vec3(0,-1,0);}
    else if(face==3u){offset=vec3(uv.y,1,uv.x);vNormal=vec3(0,1,0);}
    else if(face==4u){offset=vec3(uv.y,uv.x,0);vNormal=vec3(0,0,-1);}
    else {offset=vec3(uv.x,uv.y,1);vNormal=vec3(0,0,1);}
    // LOD2 may fill one missing voxel in a 2^3 cell. The packed XOR
    // offset points at a real representative voxel, independent of current
    // chunk LOD flags (an old mesh can still be in flight).
    uint rep=packedValue.y>>29u;
    uint rx=x^(rep&1u),ry=y^((rep>>1u)&1u),rz=z^((rep>>2u)&1u);
    uint index=(ry*32u+rz)*32u+rx;
    ChunkSlot own=slots[slot];
    uint metaBase=voxelIndirect(own.origin.w)?uint(own.zb.z):slot*8192u;
    uint packedMeta=VOXEL_WORD(metadata,metaBase,index>>2u,own.origin.w,VOXEL_UNIFORM_META);
    uint meta=(packedMeta>>((index&3u)*8u))&255u;
    // The light grid belongs to the adjacent visible air cell. Read across
    // chunk boundaries through the already resident neighboring block slot.
    ivec3 front=ivec3(int(x),int(y),int(z));
    if(face==0u)--front.x;
    else if(face==1u)++front.x;
    else if(face==2u)--front.y;
    else if(face==3u)++front.y;
    else if(face==4u)--front.z;
    else ++front.z;
    uint lightSlot=slot;
    if(any(lessThan(front,ivec3(0))) || any(greaterThanEqual(front,ivec3(32)))) {
        ChunkSlot chunk=slots[slot];
        int blockBase=-1;
        if(face==0u)blockBase=chunk.xa.x;
        else if(face==1u)blockBase=chunk.xa.y;
        else if(face==2u)blockBase=chunk.xa.z;
        else if(face==3u)blockBase=chunk.xa.w;
        else if(face==4u)blockBase=chunk.zb.x;
        else blockBase=chunk.zb.y;
        if(blockBase>=0)lightSlot=voxelNeighborSlot(chunk.origin.w,blockBase);
        else front=ivec3(int(x),int(y),int(z));
        front=(front+ivec3(32))%32;
    }
    uint lightIndex=uint((front.y*32+front.z)*32+front.x);
    uint packedLight;
    if(voxelIndirect(own.origin.w)){
        ChunkSlot lit=slots[lightSlot];
        packedLight=VOXEL_WORD(lightWords,lit.zb.w,lightIndex>>2u,lit.origin.w,VOXEL_UNIFORM_LIGHT);
    }else packedLight=lightWords[lightSlot*8192u+(lightIndex>>2u)];
    vPackedLight=(packedLight>>((lightIndex&3u)*8u))&255u;
    Material material=materials[id];
    const uint toBlockFace[6]=uint[6](1u,0u,3u,2u,5u,4u);
    FaceUv textureFace=faceUvs[material.faceBase+meta*material.metaStride+toBlockFace[face]];
    // Express repeated tile-local UV in the fragment shader, not by stretching
    // the atlas rectangle over the merged world-space quad.
    if(textureFace.options.x!=0u)uv=vec2(uv.y,1.0-uv.x);
    vTileUv=uv;
    vAtlasRect=textureFace.uv;
    vAtlasRect.y=1.0-vAtlasRect.y;
    vAtlasRect.w=1.0-vAtlasRect.w;
    localPosition=p+offset;chunkSlot=slot;
    worldPosition=vec3(slots[slot].origin.xyz)+localPosition;
    gl_Position=camera.vp*vec4(worldPosition,1.0);
}
