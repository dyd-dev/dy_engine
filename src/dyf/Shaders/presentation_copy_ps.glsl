#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
layout(set=0,binding=0) uniform texture2D sourceImage;
layout(set=0,binding=1) uniform sampler sourceSampler;
void main()
{
    color=textureLod(sampler2D(sourceImage,sourceSampler),uv,0);
}
