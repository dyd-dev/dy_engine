#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform DrawConstants
{
    mat4 worldMatrix;
    vec4 baseColor;
    uint baseColorTextureIndex;
} gDraw;

layout(set = 0, binding = 0) uniform sampler2D gTextures[1024];

void main()
{
    outColor = texture(gTextures[gDraw.baseColorTextureIndex], inUV) * gDraw.baseColor;
}
