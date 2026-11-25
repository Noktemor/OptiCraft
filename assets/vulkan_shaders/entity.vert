#version 450
layout(location=0) in vec3 pos;
layout(location=1) in vec3 normal;
layout(location=2) in vec2 uv;
layout(location=3) in ivec4 joints;
layout(location=4) in vec4 weights;
struct Pose {mat4 model;mat4 bones[64];vec4 options;};
layout(std430,set=0,binding=0) readonly buffer Poses {Pose poses[];};
layout(push_constant) uniform View {mat4 vp; vec4 eyeAmbient; vec4 fogColorStart; vec4 fogEnd; vec4 heldLight;} camera;
layout(location=0) out vec2 texCoord;
layout(location=1) out vec3 n;
layout(location=2) out vec3 worldPos;
layout(location=3) flat out vec2 lighting;
void main(){
    uint instance=uint(gl_InstanceIndex);
    mat4 model=poses[instance].model;
    vec4 options=poses[instance].options;
    mat4 skin=mat4(1.0);
    if(options.x>0.0){
        skin=mat4(0.0);
        float sum=0.0;
        for(int j=0;j<4;++j)if(weights[j]>0.0 && joints[j]>=0 && joints[j]<int(options.x)){
            skin+=poses[instance].bones[joints[j]]*weights[j];sum+=weights[j];
        }
        if(sum<=0.0)skin=mat4(1.0);
    }
    vec4 p=model*skin*vec4(pos,1.0);
    // Mob model transform has uniform scale; normalize after interpolation.
    n=mat3(model)*mat3(skin)*normal;
    texCoord=uv;worldPos=p.xyz;lighting=options.yz;
    gl_Position=camera.vp*p;
}
