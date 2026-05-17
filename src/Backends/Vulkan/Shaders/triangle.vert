#version 450
#extension GL_GOOGLE_include_directive : require

#include "RHI/RendererShaderLayout.inc"

layout(location = 0) out vec2 fragUv;
layout(location = 1) out vec3 fragWorldPosition;
layout(location = 2) out vec3 fragNormal;
layout(location = 3) out vec4 fragTangent;
layout(location = 4) out vec4 fragLightSpacePosition;

layout(push_constant) uniform DrawConstants {
    mat4 viewProjectionMatrix;
    mat4 modelMatrix;
    float drawMode;
    uint firstIndex;
    int vertexOffset;
    uint firstVertex;
    vec3 emissiveColor;
    vec4 baseColor;
    vec4 materialParams;
} pushConstants;

layout(std430, set = 0, binding = DY_RENDERER_BINDING_VERTEX_STORAGE) readonly buffer VertexStorage {
    float vertices[];
} vertexStorage;

layout(std430, set = 0, binding = DY_RENDERER_BINDING_INDEX_STORAGE) readonly buffer IndexStorage {
    uint indices[];
} indexStorage;

layout(set = 0, binding = DY_RENDERER_BINDING_SHADOW_MATRIX) uniform ShadowMatrix {
    mat4 lightViewProjectionMatrix;
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
    vec4 worldPosition = pushConstants.modelMatrix * vec4(vertex.position, 1.0);
    gl_Position = pushConstants.viewProjectionMatrix * worldPosition;
    fragUv = vertex.uv;
    fragWorldPosition = worldPosition.xyz;
    fragNormal = normalize(mat3(pushConstants.modelMatrix) * vertex.normal);
    fragTangent = vec4(normalize(mat3(pushConstants.modelMatrix) * vertex.tangent.xyz), vertex.tangent.w);
    fragLightSpacePosition = shadowMatrix.lightViewProjectionMatrix * worldPosition;
}
