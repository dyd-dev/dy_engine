#version 450

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    mat4 model;
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
    uint indexBase = uint(model[0][3]);
    float drawMode = model[3][3];
    model[0][3] = 0.0;
    model[3][3] = 1.0;

    if (drawMode < 0.0 || drawMode > 1.5) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }

    uint vertexIndex = indexStorage.indices[indexBase + uint(gl_VertexIndex)];
    vec4 worldPosition = model * vec4(LoadPosition(vertexIndex), 1.0);
    gl_Position = shadowMatrix.lightViewProj * worldPosition;
}
