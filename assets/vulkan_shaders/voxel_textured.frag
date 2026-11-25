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
// Sample a real cell from the current light grid. Missing pages and opaque
// cells are excluded from the corner average, not interpreted as black air.
// Return state: 0=missing, 1=light-bearing, 2=opaque.
uint sampleLight(uint slot,ivec3 p,out vec2 light){
    light=vec2(0);
    for(int axis=0;axis<3;++axis){
        if(p[axis]>=0 && p[axis]<32)continue;
        ChunkSlot c=slots[slot];
        int side=axis*2+int(p[axis]>=32);
        int base=side<4?c.xa[side]:c.zb[side-4];
        if(base<0)return 0u;
        slot=voxelNeighborSlot(c.origin.w,base);
        if(slot>=uint(slots.length()))return 0u;
        p[axis]=(p[axis]+32)%32;
    }
    uint i=voxelIndex(p);
    ChunkSlot cell=slots[slot];
    uint word=VOXEL_WORD(blocks,cell.source.x,i>>1u,cell.origin.w,VOXEL_UNIFORM_BLOCK);
    uint id=(word>>((i&1u)*16u))&65535u;
    if((materials[id].flags&32u)!=0u)return 2u;
    uint l=lightAt(slot,p);
    light=vec2(float(l>>4u),float(l&15u))/15.0;
    return 1u;
}
// Nine samples in the face's adjacent plane, reused by the four corners.
// Interpolate corner averages and AO across each real voxel of a greedy quad.
void smoothFaceLight(float range,inout vec2 light,out float ao){
    ao=1.0;
    if(range>=32.0)return;
    ivec3 n=ivec3(round(vNormal));
    int axis=n.x!=0?0:(n.y!=0?1:2);
    int u=(axis+1)%3,v=(axis+2)%3;
    ivec3 cell=clamp(ivec3(floor(localPosition-vec3(n)*0.001)),ivec3(0),ivec3(31));
    vec2 f=clamp(vec2(localPosition[u]-float(cell[u]),localPosition[v]-float(cell[v])),0.0,1.0);
    vec2 samples[9];uint states[9];
    for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x){
        ivec3 q=cell+n;q[u]+=x;q[v]+=y;
        int k=(y+1)*3+x+1;
        states[k]=sampleLight(chunkSlot,q,samples[k]);
    }
    vec2 corners[4];float cornerAO[4];
    for(int y=0;y<2;++y)for(int x=0;x<2;++x){
        int du=x==0?-1:1,dv=y==0?-3:3;
        int a=4+du,b=4+dv,c=4+du+dv;
        int indices[4]=int[4](4,a,b,c);
        vec2 sum=vec2(0);float count=0.0;
        for(int j=0;j<4;++j){int k=indices[j];if(states[k]==1u){sum+=samples[k];count+=1.0;}}
        int k=y*2+x;
        corners[k]=count>0.0?sum/count:light;
        bool sa=states[a]==2u,sb=states[b]==2u,sc=states[c]==2u;
        float level=sa&&sb?0.0:3.0-float(int(sa)+int(sb)+int(sc));
        cornerAO[k]=mix(0.45,1.0,level/3.0);
    }
    float fade=1.0-smoothstep(20.0,32.0,range);
    vec2 smoothLight=mix(mix(corners[0],corners[1],f.x),mix(corners[2],corners[3],f.x),f.y);
    light=mix(light,smoothLight,fade);
    ao=mix(1.0,mix(mix(cornerAO[0],cornerAO[1],f.x),mix(cornerAO[2],cornerAO[3],f.x),f.y),fade);
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
    vec2 light=vec2(float((packedLight>>4u)&15u),float(packedLight&15u))/15.0;
    float range=distance(worldPosition,camera.eyeAmbient.xyz),ao;
    smoothFaceLight(range,light,ao);
    vec3 sun=camera.fogEnd.yzw;
    float direct=mix(1.0,mix(0.80,1.08,max(dot(vNormal,sun),0.0)),smoothstep(-0.12,0.10,sun.y));
    float brightness=max(max(light.x*camera.eyeAmbient.w,light.y)*direct,0.32);
    float faceShade=vNormal.y>0.9?1.0:vNormal.y<-0.9?0.5:0.7;
    vec3 shade=faceShade*max(vec3(brightness),dynamicLight(worldPosition,vNormal,camera.eyeAmbient.xyz,camera.heldLight))*ao;
    outColor=vec4(mix(texel.rgb*shade,camera.fogColorStart.rgb,
        smoothstep(camera.fogColorStart.w,camera.fogEnd.x,range)),1.0);
}
