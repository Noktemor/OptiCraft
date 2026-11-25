#version 450
#extension GL_GOOGLE_include_directive : require
#include "dynamic_light.glsl"
#include "voxel_address.glsl"
layout(set=1,binding=0) uniform sampler2D atlas;
layout(location=0) in vec2 texCoord;
layout(location=1) in vec3 normal;
layout(location=2) flat in uint kind;
layout(location=3) flat in uint isBlended;
layout(location=4) flat in uint packedLight;
layout(constant_id=0) const int ALPHA_PASS=0;
layout(location=0) out vec4 color;
layout(location=5) in vec3 worldPosition;
layout(push_constant) uniform Projection {mat4 vp; vec4 eyeAmbient; vec4 fogColorStart; vec4 fogEnd; vec4 heldLight;} projection;
layout(location=6) in vec3 localPosition;
layout(location=7) flat in uint chunkSlot;
struct MiscSlot {uvec4 source; ivec4 origin; ivec4 neighborA; ivec4 neighborB; ivec4 metaA; ivec4 metaB;};
struct Material {uint faceBase;uint metaStride;uint shape;uint flags;};
layout(std430,set=0,binding=0) readonly buffer BlockWords {uint blocks[];};
layout(std430,set=0,binding=2) readonly buffer Slots {MiscSlot slots[];};
layout(std430,set=0,binding=3) readonly buffer Materials {Material materials[];};
bool occludes(uint slot,ivec3 p){
    for(int axis=0;axis<3;++axis){
        if(p[axis]>=0 && p[axis]<32)continue;
        MiscSlot c=slots[slot];
        int side=axis*2+int(p[axis]>=32);
        int base=side<4?c.neighborA[side]:c.neighborB[side-4];
        if(base<0)return false;
        slot=voxelNeighborSlot(c.origin.w,base);
        if(slot>=uint(slots.length()))return false;
        p[axis]=(p[axis]+32)%32;
    }
    uint i=uint((p.y*32+p.z)*32+p.x);
    MiscSlot cell=slots[slot];
    uint word=VOXEL_WORD(blocks,cell.source.x,i>>1u,cell.origin.w,VOXEL_UNIFORM_BLOCK);
    uint id=(word>>((i&1u)*16u))&65535u;
    return (materials[id].flags&32u)!=0u;
}
// Approximate smooth corner AO using the nearest corner's three occupancy
// samples. Operates on original voxels even inside a huge greedy rectangle.
// Fade out from 20 to 32 blocks to bound the extra fragment memory traffic.
float ambientOcclusion(float range){
    if(range>=32.0)return 1.0;
    ivec3 n=ivec3(round(normal));
    int axis=n.x!=0?0:(n.y!=0?1:2);
    int u=(axis+1)%3,v=(axis+2)%3;
    vec3 p=localPosition+vec3(n)*0.001;
    ivec3 front=ivec3(floor(p));
    vec3 f=fract(p);
    ivec3 du=ivec3(0),dv=ivec3(0);
    du[u]=f[u]<0.5?-1:1;dv[v]=f[v]<0.5?-1:1;
    bool a=occludes(chunkSlot,front+du),b=occludes(chunkSlot,front+dv);
    bool c=occludes(chunkSlot,front+du+dv);
    float dark=a&&b?3.0:float(int(a)+int(b)+int(c));
    float weight=abs(2.0*f[u]-1.0)*abs(2.0*f[v]-1.0);
    // Side occlusion remains visible along an edge, not only at the corner.
    float edges=max(a?abs(2.0*f[u]-1.0):0.0,b?abs(2.0*f[v]-1.0):0.0);
    float amount=max(dark/3.0*weight,edges*0.45);
    return 1.0-0.35*amount*(1.0-smoothstep(20.0,32.0,range));
}
void main(){
    // Draw-list partition selects the class before graphics. Keep a safety
    // guard and support diagnostic legacy mode. Alpha does not write depth.
    if ((isBlended != 0u) != (ALPHA_PASS != 0)) discard;
    vec4 c=texture(atlas,texCoord);
    if(c.a<0.1)discard;
    float sky=float((packedLight>>4u)&15u)/15.0;
    float block=float(packedLight&15u)/15.0;
    vec3 sun=projection.fogEnd.yzw;
    float direct=sun.y>0.0?0.75+0.25*max(dot(normal,sun),0.0):1.0;
    float brightness=max(max(sky*projection.eyeAmbient.w*direct,block),0.08);
    vec3 shade=(0.75+0.25*abs(normal.y))*max(vec3(brightness),dynamicLight(worldPosition,normal,projection.eyeAmbient.xyz,projection.heldLight));
    if(isBlended==0u && (kind&4u)==0u)shade*=ambientOcclusion(distance(worldPosition,projection.eyeAmbient.xyz));
    color=vec4(mix(c.rgb*shade,projection.fogColorStart.rgb,
        smoothstep(projection.fogColorStart.w,projection.fogEnd.x,distance(worldPosition,projection.eyeAmbient.xyz))),c.a);
}
