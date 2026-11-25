#version 450
#extension GL_GOOGLE_include_directive : require
#include "dynamic_light.glsl"
layout(set=1,binding=0) uniform sampler2D albedo;
layout(location=0) in vec2 texCoord;
layout(location=1) in vec3 n;
layout(location=2) in vec3 worldPos;
layout(location=3) flat in vec2 lighting;
layout(location=0) out vec4 color;
layout(push_constant) uniform View {mat4 vp; vec4 eyeAmbient; vec4 fogColorStart; vec4 fogEnd; vec4 heldLight;} camera;
void main(){
    vec4 c=texture(albedo,texCoord);if(c.a<0.1)discard;
    float diffuse=max(dot(normalize(n),normalize(vec3(0.3,1.0,0.4))),0.0);
    vec3 lit=c.rgb*max(vec3((0.35+0.65*diffuse)*max(lighting.y,0.4)),dynamicLight(worldPos,normalize(n),camera.eyeAmbient.xyz,camera.heldLight));
    lit=mix(lit,vec3(1.0,0.15,0.15),clamp(lighting.x,0.0,1.0)*0.55);
    color=vec4(mix(lit,camera.fogColorStart.rgb,smoothstep(camera.fogColorStart.w,camera.fogEnd.x,distance(worldPos,camera.eyeAmbient.xyz))),c.a);
}
