#version 450

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec3 fragNormal;

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    mat4 model;
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
    uint indexBase = uint(model[0][3]);
    model[0][3] = 0.0;

    uint vertexIndex = indexStorage.indices[indexBase + uint(gl_VertexIndex)];
    Vertex vertex = LoadVertex(vertexIndex);
    gl_Position = pushConstants.viewProj * model * vec4(vertex.position, 1.0);
    fragUV = vertex.uv;
    // World space normal
    fragNormal = normalize(mat3(model) * vertex.normal);
}
