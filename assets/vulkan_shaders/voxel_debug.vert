#version 450
// Diagnostic vertex-pulling graphics path. The production material/atlas/lighting
// pipeline will replace the fragment attributes; voxel geometry is GPU-produced.
struct ChunkSlot {
    uvec4 source;
    ivec4 origin;
    ivec4 neighborA;
    ivec4 neighborB;
};
layout(std430, set=0, binding=1) readonly buffer ChunkTable { ChunkSlot slots[]; };
layout(std430, set=0, binding=3) readonly buffer PackedFaces { uvec2 faces[]; };
layout(push_constant) uniform ViewParams { mat4 viewProjection; } viewParams;
layout(location=0) out vec3 vNormal;
layout(location=1) flat out uint vMaterial;
const vec2 corners[6] = vec2[6](vec2(0,0),vec2(1,0),vec2(1,1),
                                  vec2(0,0),vec2(1,1),vec2(0,1));
void main() {
    uvec2 packedValue = faces[gl_InstanceIndex];
    uint face = (packedValue.x >> 15u) & 7u;
    uint material = ((packedValue.x >> 18u) & 16383u) | ((packedValue.y & 3u) << 14u);
    uint slot = (packedValue.y >> 2u) & 65535u;
    float w=float(((packedValue.y>>18u)&31u)+1u), h=float(((packedValue.y>>23u)&31u)+1u);
    ivec3 p = ivec3(int(packedValue.x & 31u), int((packedValue.x >> 5u) & 31u),
                    int((packedValue.x >> 10u) & 31u));
    vec2 uv = corners[gl_VertexIndex];
    if(face==0u || face==2u || face==5u)uv*=vec2(w,h);else uv*=vec2(h,w);
    vec3 offset;
    if      (face == 0u) { offset = vec3(0,uv.y,uv.x); vNormal=vec3(-1,0,0); }
    else if (face == 1u) { offset = vec3(1,uv.x,uv.y); vNormal=vec3( 1,0,0); }
    else if (face == 2u) { offset = vec3(uv.x,0,uv.y); vNormal=vec3(0,-1,0); }
    else if (face == 3u) { offset = vec3(uv.y,1,uv.x); vNormal=vec3(0, 1,0); }
    else if (face == 4u) { offset = vec3(uv.y,uv.x,0); vNormal=vec3(0,0,-1); }
    else                 { offset = vec3(uv.x,uv.y,1); vNormal=vec3(0,0, 1); }
    vMaterial = material;
    vec3 world = vec3(slots[slot].origin.xyz + p) + offset;
    gl_Position = viewParams.viewProjection * vec4(world,1);
}
