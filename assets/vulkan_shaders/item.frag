#version 450
#extension GL_GOOGLE_include_directive : require
#include "dynamic_light.glsl"
layout(set=0,binding=0) uniform sampler2D atlas;
layout(location=0) in vec2 texCoord;
layout(location=1) in vec3 n;
layout(location=2) in float light;
layout(location=3) in vec3 worldPos;
layout(location=0) out vec4 color;
layout(push_constant) uniform View {mat4 vp; vec4 eyeAmbient; vec4 fogColorStart; vec4 fogEnd; vec4 heldLight;} camera;
void main(){
    vec4 c=texture(atlas,texCoord);if(c.a<0.1)discard;
    float packedValue=floor(light+0.5);
    float brightness=max(max(floor(packedValue/16.0)/15.0*camera.eyeAmbient.w,mod(packedValue,16.0)/15.0),0.34);
    float shade=n.y>0.9?1.0:(n.y<-0.9?0.55:0.8);
    color=vec4(mix(c.rgb*max(vec3(brightness),dynamicLight(worldPos,n,camera.eyeAmbient.xyz,camera.heldLight))*shade,camera.fogColorStart.rgb,smoothstep(camera.fogColorStart.w,camera.fogEnd.x,distance(worldPos,camera.eyeAmbient.xyz))),c.a);
}
