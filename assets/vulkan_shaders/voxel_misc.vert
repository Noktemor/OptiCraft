#version 450
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
layout(push_constant) uniform Projection {mat4 vp;} projection;
layout(location=0) out vec2 texCoord;
layout(location=1) out vec3 normal;
layout(location=2) flat out uint kind;
layout(location=3) flat out uint isBlended;
layout(location=4) flat out uint packedLight;
const vec2 CORNERS[6] = vec2[6](vec2(0,0),vec2(1,0),vec2(1,1),
                                          vec2(0,0),vec2(1,1),vec2(0,1));
float coord(uint bits,uint axis){return float((bits>>(7u*axis))&127u)/64.0;}
void main(){
    uvec4 a=quads[gl_InstanceIndex*2];
    uvec4 b=quads[gl_InstanceIndex*2+1];
    uint face=a.w&15u, meta=(a.w>>8u)&255u;
    kind=(a.w>>16u)&31u;
    uint idx=a.z;
    uint packed=lights[a.x*8192u+(idx>>2u)];
    packedLight=(packed>>((idx&3u)*8u))&255u;
    vec3 cell=vec3(float(idx&31u),float(idx>>10u),float((idx>>5u)&31u));
    vec3 low=vec3(coord(b.x,0u),coord(b.x,1u),coord(b.x,2u));
    vec3 high=vec3(coord(b.y,0u),coord(b.y,1u),coord(b.y,2u));
    vec2 uv=CORNERS[gl_VertexIndex];
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
            uint packed=(c<2u)?b.z:b.w;
            uint h=(packed>>(16u*(c&1u)))&65535u;
            p.y=float(h)/65535.0;
        }
    }
    Material mat=materials[a.y];
    isBlended = ((mat.flags & 128u) != 0u) ? 1u : 0u;
    uint uvFace=min(face,5u);
    FaceUv rect=faces[mat.faceBase+meta*mat.metaStride+uvFace];
    if(rect.options.x!=0u) uv=vec2(uv.y,1.0-uv.x);
    // Game atlas UVs are authored for OpenGL textures loaded with STB vertical
    // flip. Vulkan uploads the PNG unchanged; invert V at sampling instead of
    // performing a full-frame CPU pixel swizzle during startup.
    texCoord=mix(rect.uv.xy,rect.uv.zw,uv);
    texCoord.y=1.0-texCoord.y;
    gl_Position=projection.vp*vec4(vec3(slots[a.x].origin.xyz)+cell+p,1.0);
}
