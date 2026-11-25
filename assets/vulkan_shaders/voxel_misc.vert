#version 450
#extension GL_GOOGLE_include_directive : require
#include "voxel_address.glsl"
// One 32-byte GPU-produced record -> two triangles. Used for every non-opaque-
// cube placed block including ALL game Block_Shape variants, plants and fluids.
struct MiscSlot {uvec4 source; ivec4 origin; ivec4 neighborA; ivec4 neighborB;
                 ivec4 metaA; ivec4 metaB;};
struct Material {uint faceBase; uint metaStride; uint shape; uint flags;};
struct FaceUv {vec4 uv; uvec4 options;};
layout(std430,set=0,binding=2) readonly buffer Slots {MiscSlot slots[];};
layout(std430,set=0,binding=3) readonly buffer Materials {Material materials[];};
layout(std430,set=0,binding=7) readonly buffer Geometry {uvec4 quads[];};
layout(std430,set=0,binding=9) readonly buffer UvTable {FaceUv faces[];};
layout(std430,set=0,binding=10) readonly buffer LightWords {uint lights[];};
layout(std430,set=2,binding=0) readonly buffer AlphaOrder {uint alphaOrder[];};
layout(push_constant) uniform Projection {mat4 vp; vec4 eyeAmbient; vec4 fogColorStart; vec4 fogEnd; vec4 heldLight;} projection;
layout(location=0) out vec2 texCoord;
layout(location=1) out vec3 normal;
layout(location=2) flat out uint kind;
layout(location=3) flat out uint isBlended;
layout(location=4) flat out uint packedLight;
const vec2 CORNERS[6] = vec2[6](vec2(0,0),vec2(1,0),vec2(1,1),
                                          vec2(0,0),vec2(1,1),vec2(0,1));
float coord(uint bits,uint axis){return float((bits>>(7u*axis))&127u)/64.0;}
layout(location=5) out vec3 worldPosition;
layout(location=6) out vec3 localPosition;
layout(location=7) flat out uint chunkSlot;
layout(constant_id=0) const int ALPHA_PASS=0;
void main(){
    // Draw-list partition ABI: firstVertex=0 for direct depth/alpha,
    // firstVertex=6 for indexed depth; Vulkan gl_VertexIndex includes it.
    uint vertex=uint(gl_VertexIndex);
    bool indexedDepth=vertex>=6u;
    uint quadIndex=(ALPHA_PASS!=0 || indexedDepth)?alphaOrder[gl_InstanceIndex]:uint(gl_InstanceIndex);
    uvec4 a=quads[quadIndex*2u];
    Material mat=materials[a.y];
    isBlended = ((mat.flags & 128u) != 0u) ? 1u : 0u;
    // Safety guard; normally the partition excludes the other class entirely.
    // Also supports the diagnostic legacy unpartitioned depth list.
    if ((isBlended != 0u) != (ALPHA_PASS != 0)) {
        gl_Position=vec4(2.0,0.0,0.0,1.0);
        texCoord=vec2(0);normal=vec3(0);kind=0u;packedLight=0u;worldPosition=vec3(0);localPosition=vec3(0);chunkSlot=0u;
        return;
    }
    uvec4 b=quads[quadIndex*2u+1u];
    uint face=a.w&15u, meta=(a.w>>8u)&255u;
    kind=(a.w>>16u)&31u;
    uint idx=a.z;
    MiscSlot cellSlot=slots[a.x];
    uint lightBase=voxelIndirect(cellSlot.origin.w)?uint(cellSlot.neighborB.z):a.x*8192u;
    uint packedValue=VOXEL_WORD(lights,lightBase,idx>>2u,cellSlot.origin.w,VOXEL_UNIFORM_LIGHT);
    packedLight=(packedValue>>((idx&3u)*8u))&255u;
    vec3 cell=vec3(float(idx&31u),float(idx>>10u),float((idx>>5u)&31u));
    vec3 low=vec3(coord(b.x,0u),coord(b.x,1u),coord(b.x,2u));
    vec3 high=vec3(coord(b.y,0u),coord(b.y,1u),coord(b.y,2u));
    vec2 uv=CORNERS[indexedDepth?vertex-6u:vertex];
    vec3 p;
    if(face>=6u){
        p=vec3(face==6u?uv.x:1.0-uv.x,uv.y,uv.x);
        normal=vec3(0,1,0); // two-sided flora pass
    }else{
        uint axis=face>>1u;
        uint u=(axis+1u)%3u,v=(axis+2u)%3u;
        p=low;
        p[axis]=(face&1u)==0u?high[axis]:low[axis];
        p[u]=mix(low[u],high[u],uv.x);
        p[v]=mix(low[v],high[v],uv.y);
        normal=vec3(0);
        normal[axis]=(face&1u)==0u?1.0:-1.0;
        // Fluid top is affected by level metadata; the block below the surface
        // remains opaque-free, while the upper faces follow the fluid height.
        if((kind&8u)!=0u && p.y>0.0){
            uint c=uint(uv.x>0.5)+2u*uint(uv.y>0.5);
            uint packedValue=(c<2u)?b.z:b.w;
            uint h=(packedValue>>(16u*(c&1u)))&65535u;
            p.y=float(h)/65535.0;
        }
    }
    uint uvFace=min(face,5u);
    FaceUv rect=faces[mat.faceBase+meta*mat.metaStride+uvFace];
    if(rect.options.x!=0u) uv=vec2(uv.y,1.0-uv.x);
    // Game atlas UVs are authored for OpenGL textures loaded with STB vertical
    // flip. Vulkan uploads the PNG unchanged; invert V at sampling instead of
    // performing a full-frame CPU pixel swizzle during startup.
    texCoord=mix(rect.uv.xy,rect.uv.zw,uv);
    texCoord.y=1.0-texCoord.y;
    localPosition=cell+p;chunkSlot=a.x;
    worldPosition=vec3(slots[a.x].origin.xyz)+localPosition;
    gl_Position=projection.vp*vec4(worldPosition,1.0);
}
