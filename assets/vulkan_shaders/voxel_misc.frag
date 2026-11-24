#version 450
layout(set=1,binding=0) uniform sampler2D atlas;
layout(location=0) in vec2 texCoord;
layout(location=1) in vec3 normal;
layout(location=2) flat in uint kind;
layout(location=3) flat in uint isBlended;
layout(location=4) flat in uint packedLight;
layout(constant_id=0) const int ALPHA_PASS=0;
layout(location=0) out vec4 color;
void main(){
    // Same GPU quad array is drawn in two passes, but only its own class
    // survives fragment shading. The alpha pass does not write depth.
    if ((isBlended != 0u) != (ALPHA_PASS != 0)) discard;
    vec4 c=texture(atlas,texCoord);
    if(c.a<0.1)discard;
    float sky=float((packedLight>>4u)&15u)/15.0;
    float block=float(packedLight&15u)/15.0;
    float brightness=max(max(sky,block),0.20);
    float shade=(0.75+0.25*abs(normal.y))*brightness;
    color=vec4(c.rgb*shade,c.a);
}
