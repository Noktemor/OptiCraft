#version 450
layout(location=0) in vec3 vNormal;
layout(location=1) flat in uint vMaterial;
layout(location=0) out vec4 outColor;
void main() {
    uint hash = vMaterial * 747796405u + 2891336453u;
    hash ^= hash >> 16u;
    vec3 base = vec3(0.35 + float(hash & 255u)/512.0,
                     0.35 + float((hash>>8u)&255u)/512.0,
                     0.35 + float((hash>>16u)&255u)/512.0);
    float light = 0.48 + 0.52*max(0.0,dot(normalize(vNormal),normalize(vec3(0.5,1,0.7))));
    outColor = vec4(base*light,1);
}
