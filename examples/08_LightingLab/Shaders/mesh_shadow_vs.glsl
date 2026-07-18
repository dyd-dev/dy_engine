#version 450
#extension GL_GOOGLE_include_directive : require

#include "Graphics/RendererShaderLayout.inc"

layout(std140, set = 0, binding = DY_VULKAN_BINDING_DRAW_CONSTANTS) uniform VulkanDrawConstants {
    mat4 viewProjectionMatrix;
    mat4 modelMatrix;
    float drawMode;
    uint firstIndex;
    int vertexOffset;
    uint firstVertex;
    vec3 emissiveColor;
    float shadowViewIndex;
    vec4 baseColor;
    vec4 materialParams;
} pushConstants;

layout(std140, set = 0, binding = DY_RENDERER_BINDING_SHADOW_MATRIX) uniform ShadowMatrix {
    mat4 lightViewProjectionMatrices[6];
    vec4 cascadeSplits;
    vec4 shadowInfo;
    vec4 pcssParams;
    mat4 cameraViewMatrix;
} shadowMatrix;

layout(std430, set = 0, binding = DY_RENDERER_BINDING_VERTEX_STORAGE) readonly buffer VertexStorage {
    float vertices[];
} vertexStorage;

layout(std430, set = 0, binding = DY_RENDERER_BINDING_INDEX_STORAGE) readonly buffer IndexStorage {
    uint indices[];
} indexStorage;

vec3 LoadPosition(uint vertexIndex) {
    uint base = vertexIndex * DY_RENDERER_VERTEX_FLOAT_COUNT;
    return vec3(
        vertexStorage.vertices[base + 0u],
        vertexStorage.vertices[base + 1u],
        vertexStorage.vertices[base + 2u]);
}

void main() {
    if ((int(pushConstants.drawMode + 0.5) & DY_RENDERER_TEXTURE_FLAG_CAST_SHADOW) == 0) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }

    int resolvedVertexIndex = int(indexStorage.indices[pushConstants.firstIndex + uint(gl_VertexIndex)]) + pushConstants.vertexOffset;
    vec4 worldPosition = pushConstants.modelMatrix * vec4(LoadPosition(uint(resolvedVertexIndex)), 1.0);
    int shadowView = clamp(int(pushConstants.shadowViewIndex + 0.5), 0, 5);
    gl_Position = shadowMatrix.lightViewProjectionMatrices[shadowView] * worldPosition;
}
