#version 450
layout(location=0) in vec3 pos;
layout(location=1) in vec2 uv;
layout(location=2) in vec3 normal;
layout(location=3) in float packedLight;
layout(location=0) out vec2 texCoord;
layout(location=1) out vec3 n;
layout(location=2) out float light;
layout(location=3) out vec3 worldPos;
layout(push_constant) uniform View {mat4 vp; vec4 eyeAmbient; vec4 fogColorStart; vec4 fogEnd; vec4 heldLight;} camera;
void main(){texCoord=vec2(uv.x,1.0-uv.y);n=normal;light=packedLight;worldPos=pos;gl_Position=camera.vp*vec4(pos,1.0);}
