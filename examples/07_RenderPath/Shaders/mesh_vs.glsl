#version 450
#extension GL_GOOGLE_include_directive : require

#include "Graphics/RendererShaderLayout.inc"

layout(location = 0) out vec2 fragUv;
layout(location = 1) out vec3 fragWorldPosition;
layout(location = 2) out vec3 fragNormal;
layout(location = 3) out vec4 fragTangent;
layout(location = 4) out vec4 fragLightSpacePosition;

layout(std140, set = 0, binding = DY_VULKAN_BINDING_DRAW_CONSTANTS) uniform VulkanDrawConstants {
    mat4 viewProjectionMatrix;
    mat4 modelMatrix;
    float drawMode;
    uint firstIndex;
    int vertexOffset;
    uint firstVertex;
    vec3 emissiveColor;
    float baseColorTextureIndex;
    vec4 baseColor;
    vec4 materialParams;
} pushConstants;

layout(std430, set = 0, binding = DY_RENDERER_BINDING_VERTEX_STORAGE) readonly buffer VertexStorage {
    float vertices[];
} vertexStorage;

layout(std430, set = 0, binding = DY_RENDERER_BINDING_INDEX_STORAGE) readonly buffer IndexStorage {
    uint indices[];
} indexStorage;

layout(std430, set = 0, binding = DY_RENDERER_BINDING_BINDLESS_TRANSFORM_STORAGE) readonly buffer InstanceTransformStorage {
    mat4 modelMatrices[];
} instanceTransforms;

layout(std140, set = 0, binding = DY_RENDERER_BINDING_SHADOW_MATRIX) uniform ShadowMatrix {
    mat4 lightViewProjectionMatrices[6];
    vec4 cascadeSplits;
    vec4 shadowInfo;
    vec4 pcssParams;
    mat4 cameraViewMatrix;
} shadowMatrix;

struct Vertex {
    vec3 position;
    vec3 normal;
    vec2 uv;
    vec4 tangent;
};

Vertex LoadVertex(uint vertexIndex) {
    uint base = vertexIndex * DY_RENDERER_VERTEX_FLOAT_COUNT;
    Vertex vertex;
    vertex.position = vec3(
        vertexStorage.vertices[base + 0u],
        vertexStorage.vertices[base + 1u],
        vertexStorage.vertices[base + 2u]);
    vertex.normal = vec3(
        vertexStorage.vertices[base + 3u],
        vertexStorage.vertices[base + 4u],
        vertexStorage.vertices[base + 5u]);
    vertex.uv = vec2(
        vertexStorage.vertices[base + 6u],
        vertexStorage.vertices[base + 7u]);
    vertex.tangent = vec4(
        vertexStorage.vertices[base + 8u],
        vertexStorage.vertices[base + 9u],
        vertexStorage.vertices[base + 10u],
        vertexStorage.vertices[base + 11u]);
    return vertex;
}

void main() {
    int resolvedVertexIndex = int(indexStorage.indices[pushConstants.firstIndex + uint(gl_VertexIndex)]) + pushConstants.vertexOffset;
    Vertex vertex = LoadVertex(uint(resolvedVertexIndex));
    uint instanceBase = pushConstants.firstVertex;
    mat4 resolvedModelMatrix = pushConstants.modelMatrix;
    if (instanceBase != 0u) {
        resolvedModelMatrix = instanceTransforms.modelMatrices[instanceBase - 1u + gl_InstanceIndex];
    }
    vec4 worldPosition = resolvedModelMatrix * vec4(vertex.position, 1.0);
    gl_Position = pushConstants.viewProjectionMatrix * worldPosition;
    fragUv = vertex.uv;
    fragWorldPosition = worldPosition.xyz;
    mat3 model3x3 = mat3(resolvedModelMatrix);
    float modelDeterminant = determinant(model3x3);
    mat3 normalMatrix = abs(modelDeterminant) > 0.000001
        ? transpose(inverse(mat3(resolvedModelMatrix)))
        : mat3(1.0);
    fragNormal = normalize(normalMatrix * vertex.normal);
    float tangentHandedness = modelDeterminant < 0.0 ? -vertex.tangent.w : vertex.tangent.w;
    fragTangent = vec4(normalize(model3x3 * vertex.tangent.xyz), tangentHandedness);
    fragLightSpacePosition = shadowMatrix.lightViewProjectionMatrices[0] * worldPosition;
}
