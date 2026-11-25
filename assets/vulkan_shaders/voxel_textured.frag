#version 450
#extension GL_GOOGLE_include_directive : require
#include "dynamic_light.glsl"
#include "voxel_address.glsl"
layout(early_fragment_tests) in;
layout(set=1,binding=0) uniform sampler2D atlas;
layout(location=0) in vec2 vTileUv;
layout(location=2) in vec4 vAtlasRect;
layout(location=3) flat in uint vPackedLight;
layout(location=1) in vec3 vNormal;
layout(location=0) out vec4 outColor;
layout(location=4) in vec3 worldPosition;
layout(location=5) in vec3 localPosition;
layout(location=6) flat in uint chunkSlot;
layout(location=7) flat in uint surfaceLod;
struct ChunkSlot {uvec4 source; ivec4 origin; ivec4 xa; ivec4 zb;};
struct Material {uint faceBase; uint metaStride; uint shape; uint flags;};
struct FaceUv {vec4 uv;uvec4 options;};
layout(std430,set=0,binding=0) readonly buffer ChunkSlots {ChunkSlot slots[];};
layout(std430,set=0,binding=2) readonly buffer MaterialTable {Material materials[];};
layout(std430,set=0,binding=6) readonly buffer Voxels {uint blocks[];};
layout(std430,set=0,binding=3) readonly buffer FaceTable {FaceUv faceUvs[];};
layout(std430,set=0,binding=4) readonly buffer MetaBytes {uint metadata[];};
layout(std430,set=0,binding=5) readonly buffer LightBytes {uint lightWords[];};
uint voxelIndex(ivec3 p){return uint((p.y*32+p.z)*32+p.x);}
uint lightAt(uint slot,ivec3 p){
    uint i=voxelIndex(p);ChunkSlot c=slots[slot];
    uint base=voxelIndirect(c.origin.w)?uint(c.zb.w):slot*8192u;
    return (VOXEL_WORD(lightWords,base,i>>2u,c.origin.w,VOXEL_UNIFORM_LIGHT)>>((i&3u)*8u))&255u;
}
void lodSurface(out vec2 tile,out vec4 rect,out uint packedLight){
    // Geometry is still at original voxel boundaries, so even mixed material
    // planes reconstruct the exact visible voxel's texture and illumination.
    ivec3 n=ivec3(round(vNormal));
    ivec3 p=clamp(ivec3(floor(localPosition-vec3(n)*0.001)),ivec3(0),ivec3(31));
    uint i=voxelIndex(p),slot=chunkSlot;
    ChunkSlot chunk=slots[slot];
    uint id=(VOXEL_WORD(blocks,chunk.source.x,i>>1u,chunk.origin.w,VOXEL_UNIFORM_BLOCK)>>((i&1u)*16u))&65535u;
    uint metaBase=voxelIndirect(chunk.origin.w)?uint(chunk.zb.z):slot*8192u;
    uint meta=(VOXEL_WORD(metadata,metaBase,i>>2u,chunk.origin.w,VOXEL_UNIFORM_META)>>((i&3u)*8u))&255u;
    uint face=n.x!=0?uint(n.x>0):n.y!=0?2u+uint(n.y>0):4u+uint(n.z>0);
    const uint toBlockFace[6]=uint[6](1u,0u,3u,2u,5u,4u);
    // Source revisions can advance before a replacement mesh is published.
    // In that interval use the old representative face rather than indexing
    // the UV table of an air/non-cube material.
    Material mat=materials[id];
    if((mat.flags&1u)==0u){tile=vTileUv;rect=vAtlasRect;packedLight=vPackedLight;return;}
    FaceUv uv=faceUvs[mat.faceBase+meta*mat.metaStride+toBlockFace[face]];
    if(face==0u)tile=localPosition.zy;
    else if(face==1u)tile=localPosition.yz;
    else if(face==2u)tile=localPosition.xz;
    else if(face==3u)tile=localPosition.zx;
    else if(face==4u)tile=localPosition.yx;
    else tile=localPosition.xy;
    if(uv.options.x!=0u)tile=vec2(tile.y,1.0-tile.x);
    rect=uv.uv;rect.y=1.0-rect.y;rect.w=1.0-rect.w;
    ivec3 q=p+n;
    if(any(lessThan(q,ivec3(0)))||any(greaterThanEqual(q,ivec3(32)))){
        int base=face<4u?chunk.xa[face]:chunk.zb[face-4u];
        if(base<0)q=p;
        else{slot=voxelNeighborSlot(chunk.origin.w,base);q=(q+32)%32;}
    }
    packedLight=lightAt(slot,q);
}
// Follow at most three axial neighbor links for corner samples, including
// diagonals. Missing/unloaded source data is treated as non-occluding.
bool occludes(uint slot,ivec3 p){
    for(int axis=0;axis<3;++axis){
        if(p[axis]>=0 && p[axis]<32)continue;
        ChunkSlot c=slots[slot];
        int side=axis*2+int(p[axis]>=32);
        int base=side<4?c.xa[side]:c.zb[side-4];
        if(base<0)return false;
        slot=voxelNeighborSlot(c.origin.w,base);
        if(slot>=uint(slots.length()))return false;
        p[axis]=(p[axis]+32)%32;
    }
    uint i=uint((p.y*32+p.z)*32+p.x);
    ChunkSlot cell=slots[slot];
    uint word=VOXEL_WORD(blocks,cell.source.x,i>>1u,cell.origin.w,VOXEL_UNIFORM_BLOCK);
    uint id=(word>>((i&1u)*16u))&65535u;
    return (materials[id].flags&32u)!=0u;
}
// Approximate smooth corner AO using the nearest corner's three occupancy
// samples. Operates on original voxels even inside a huge greedy rectangle.
// Fade out from 20 to 32 blocks to bound the extra fragment memory traffic.
float ambientOcclusion(float range){
    if(range>=32.0)return 1.0;
    ivec3 n=ivec3(round(vNormal));
    int axis=n.x!=0?0:(n.y!=0?1:2);
    int u=(axis+1)%3,v=(axis+2)%3;
    vec3 p=localPosition+vec3(n)*0.001;
    ivec3 front=ivec3(floor(p));
    vec3 f=fract(p);
    ivec3 du=ivec3(0),dv=ivec3(0);
    du[u]=f[u]<0.5?-1:1;dv[v]=f[v]<0.5?-1:1;
    bool a=occludes(chunkSlot,front+du),b=occludes(chunkSlot,front+dv);
    // The diagonal cannot change the result when both sides occlude.
    // Avoid its potentially cross-chunk voxel/material lookup in that case.
    float dark=3.0;
    if(!(a&&b)){
        bool c=occludes(chunkSlot,front+du+dv);
        dark=float(int(a)+int(b)+int(c));
    }
    float weight=abs(2.0*f[u]-1.0)*abs(2.0*f[v]-1.0);
    // Side occlusion remains visible along an edge, not only at the corner.
    float edges=max(a?abs(2.0*f[u]-1.0):0.0,b?abs(2.0*f[v]-1.0):0.0);
    float amount=max(dark/3.0*weight,edges*0.45);
    return 1.0-0.35*amount*(1.0-smoothstep(20.0,32.0,range));
}
layout(push_constant) uniform View {mat4 vp; vec4 eyeAmbient; vec4 fogColorStart; vec4 fogEnd; vec4 heldLight;} camera;
void main(){
    // Explicit gradients avoid the implicit-derivative discontinuity at tile
    // repeats; mipmaps were built independently per atlas tile.
    vec2 tile=vTileUv;vec4 rect=vAtlasRect;uint packedLight=vPackedLight;
    if(surfaceLod!=0u)lodSurface(tile,rect,packedLight);
    vec2 scale=rect.zw-rect.xy;
    vec2 tiled=fract(tile);
    vec2 texelHalf=0.5/vec2(textureSize(atlas,0));
    vec2 uv=clamp(rect.xy+tiled*scale,
                  min(rect.xy,rect.zw)+texelHalf,
                  max(rect.xy,rect.zw)-texelHalf);
    vec4 texel=textureGrad(atlas,uv,
                           dFdx(tile)*scale,dFdy(tile)*scale);
    float sky=float((packedLight>>4u)&15u)/15.0;
    float block=float(packedLight&15u)/15.0;
    // Direct sky and propagated block light, with a small safety floor while
    // the game streams its light grid. No GI or dynamic shadow casting yet.
    vec3 sun=camera.fogEnd.yzw;
    float direct=1.0;
    if(sun.y>0.0)direct=0.75+0.25*max(dot(vNormal,sun),0.0);
    float brightness=max(max(sky*camera.eyeAmbient.w*direct,block),0.08);
    float range=distance(worldPosition,camera.eyeAmbient.xyz);
    vec3 shade=(0.68+0.32*abs(vNormal.y))*max(vec3(brightness),dynamicLight(worldPosition,vNormal,camera.eyeAmbient.xyz,camera.heldLight))*ambientOcclusion(range);
    outColor=vec4(mix(texel.rgb*shade,camera.fogColorStart.rgb,
        smoothstep(camera.fogColorStart.w,camera.fogEnd.x,range)),1.0);
}
