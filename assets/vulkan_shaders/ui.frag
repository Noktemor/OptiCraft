#version 450
layout(set=0,binding=0) uniform sampler2D atlas;
layout(location=0) in vec2 uv;
layout(location=1) in vec4 tint;
layout(location=0) out vec4 outColor;
void main(){outColor=textureLod(atlas,uv,0.0)*tint;if(outColor.a<0.004)discard;}
