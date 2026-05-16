#version 450

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec3 fragWorldPosition;
layout(location = 3) out vec4 fragTangent;

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    mat4 model;
    float drawMode;
    uint firstIndex;
    int vertexOffset;
    uint firstVertex;
    float padding;
    vec4 baseColorFactor;
    vec4 materialParams;
} pushConstants;

layout(std430, set = 0, binding = 4) readonly buffer VertexStorage {
    float vertices[];
} vertexStorage;

layout(std430, set = 0, binding = 5) readonly buffer IndexStorage {
    uint indices[];
} indexStorage;

struct Vertex {
    vec3 position;
    vec3 normal;
    vec2 uv;
    vec4 tangent;
};

Vertex LoadVertex(uint vertexIndex) {
    uint base = vertexIndex * 12u;
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
    mat4 model = pushConstants.model;

    int resolvedVertexIndex = int(indexStorage.indices[pushConstants.firstIndex + uint(gl_VertexIndex)]) + pushConstants.vertexOffset;
    uint vertexIndex = uint(resolvedVertexIndex);
    Vertex vertex = LoadVertex(vertexIndex);
    vec4 worldPosition = model * vec4(vertex.position, 1.0);
    gl_Position = pushConstants.viewProj * worldPosition;
    fragUV = vertex.uv;
    fragWorldPosition = worldPosition.xyz;
    fragNormal = normalize(mat3(model) * vertex.normal);
    fragTangent = vec4(normalize(mat3(model) * vertex.tangent.xyz), vertex.tangent.w);
}
