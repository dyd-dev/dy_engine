#version 450

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    mat4 model;
    float drawMode;
    uint firstIndex;
    int vertexOffset;
    uint firstVertex;
} pushConstants;

layout(set = 0, binding = 3) uniform ShadowMatrix {
    mat4 lightViewProj;
} shadowMatrix;

layout(std430, set = 0, binding = 4) readonly buffer VertexStorage {
    float vertices[];
} vertexStorage;

layout(std430, set = 0, binding = 5) readonly buffer IndexStorage {
    uint indices[];
} indexStorage;

vec3 LoadPosition(uint vertexIndex) {
    uint base = vertexIndex * 8u;
    return vec3(
        vertexStorage.vertices[base + 0u],
        vertexStorage.vertices[base + 1u],
        vertexStorage.vertices[base + 2u]);
}

void main() {
    mat4 model = pushConstants.model;
    float drawMode = pushConstants.drawMode;

    if (drawMode < 0.0 || drawMode > 1.5) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }

    int resolvedVertexIndex = int(indexStorage.indices[pushConstants.firstIndex + uint(gl_VertexIndex)]) + pushConstants.vertexOffset;
    uint vertexIndex = uint(resolvedVertexIndex);
    vec4 worldPosition = model * vec4(LoadPosition(vertexIndex), 1.0);
    gl_Position = shadowMatrix.lightViewProj * worldPosition;
}
