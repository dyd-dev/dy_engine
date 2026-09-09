#version 450
layout(set=0,binding=0) uniform texture2D image;
layout(set=0,binding=1) uniform sampler imageSampler;
layout(location=0) in vec2 uv;
layout(location=1) in vec4 color;
layout(location=0) out vec4 result;
void main() { result=color*texture(sampler2D(image,imageSampler),uv); }
