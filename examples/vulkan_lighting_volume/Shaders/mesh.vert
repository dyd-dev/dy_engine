#version 450
#extension GL_GOOGLE_include_directive : require

#include "RHI/RendererShaderLayout.inc"

layout(location = 0) out vec3 fragWorldPosition;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec2 fragUV;
layout(location = 3) out float fragDrawMode;
layout(location = 4) out vec4 fragLightSpacePos;

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    mat4 model;
    float drawMode;
    uint firstIndex;
    int vertexOffset;
    uint firstVertex;
} pushConstants;

// Shadow Map 변환용 Light-Space ViewProjection.
// RHI device updates this from the selected backend each frame.
layout(set = 0, binding = DY_RENDERER_BINDING_SHADOW_MATRIX) uniform ShadowMatrix {
    mat4 lightViewProj;
} shadowMatrix;

layout(std430, set = 0, binding = DY_RENDERER_BINDING_VERTEX_STORAGE) readonly buffer VertexStorage {
    float vertices[];
} vertexStorage;

layout(std430, set = 0, binding = DY_RENDERER_BINDING_INDEX_STORAGE) readonly buffer IndexStorage {
    uint indices[];
} indexStorage;

struct Vertex {
    vec3 position;
    vec3 normal;
    vec2 uv;
};

Vertex LoadVertex(uint vertexIndex) {
    uint base = vertexIndex * 8u;
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
    return vertex;
}

void main() {
    mat4 model = pushConstants.model;
    fragDrawMode = pushConstants.drawMode;

    int resolvedVertexIndex = int(indexStorage.indices[pushConstants.firstIndex + uint(gl_VertexIndex)]) + pushConstants.vertexOffset;
    uint vertexIndex = uint(resolvedVertexIndex);
    Vertex vertex = LoadVertex(vertexIndex);
    vec4 worldPosition = model * vec4(vertex.position, 1.0);
    gl_Position = pushConstants.viewProj * worldPosition;
    fragWorldPosition = worldPosition.xyz;
    fragNormal = normalize(mat3(model) * vertex.normal);
    fragUV = vertex.uv;

    // FS에서 ShadowMap UV/depth로 변환할 수 있게 light space에서의 위치를 함께 보낸다.
    fragLightSpacePos = shadowMatrix.lightViewProj * worldPosition;
}
