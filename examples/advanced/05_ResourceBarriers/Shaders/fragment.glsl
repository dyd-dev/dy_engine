#version 450
layout(location=0) in vec2 uv;layout(location=0) out vec4 result;layout(set=0,binding=0) uniform texture2D source;layout(set=0,binding=1) uniform sampler pointSampler;
void main(){result=texture(sampler2D(source,pointSampler),uv);}
