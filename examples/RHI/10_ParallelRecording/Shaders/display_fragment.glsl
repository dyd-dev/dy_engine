#version 450

layout(set = 0, binding = 0) uniform texture2D sourceImage;
layout(set = 0, binding = 1) uniform sampler imageSampler;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outputColor;

void main()
{
    outputColor = texture(sampler2D(sourceImage, imageSampler), uv);
}
