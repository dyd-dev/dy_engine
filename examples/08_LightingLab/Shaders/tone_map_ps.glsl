#version 450
#extension GL_GOOGLE_include_directive : require

#include "Graphics/RendererShaderLayout.inc"

layout(location = 0) in vec2 fragUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = DY_RENDERER_BINDING_BASE_COLOR_TEXTURE) uniform sampler2D hdrColor;

layout(std140, set = 0, binding = DY_VULKAN_BINDING_DRAW_CONSTANTS) uniform VulkanDrawConstants {
    mat4 viewProjectionMatrix;
    mat4 modelMatrix;
    float drawMode;
    uint firstIndex;
    int vertexOffset;
    uint firstVertex;
    vec4 emissiveColor;
    vec4 baseColor;
    vec4 materialParams;
    vec4 textureIndices;
} pushConstants;

vec3 AcesFitted(vec3 color) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
}

void main() {
    vec3 hdr = max(texture(hdrColor, fragUv).rgb * max(pushConstants.baseColor.x, 0.0), vec3(0.0));
    vec3 color = AcesFitted(hdr);
    if (pushConstants.baseColor.y > 0.5) {
        color = pow(color, vec3(1.0 / 2.2));
    }
    outColor = vec4(color, 1.0);
}
