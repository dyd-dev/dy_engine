#version 450

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

layout(set = 0, binding = 3) uniform ShadowMatrix {
    mat4 lightViewProjectionMatrix;
} shadowMatrix;

layout(std430, set = 0, binding = 4) readonly buffer VertexStorage {
    float vertices[];
} vertexStorage;

layout(std430, set = 0, binding = 5) readonly buffer IndexStorage {
    uint indices[];
} indexStorage;

const int CastShadowFlag = 64;

vec3 LoadPosition(uint vertexIndex) {
    uint base = vertexIndex * 12u;
    return vec3(
        vertexStorage.vertices[base + 0u],
        vertexStorage.vertices[base + 1u],
        vertexStorage.vertices[base + 2u]);
}

void main() {
    if ((int(pushConstants.drawMode + 0.5) & CastShadowFlag) == 0) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }

    int resolvedVertexIndex = int(indexStorage.indices[pushConstants.firstIndex + uint(gl_VertexIndex)]) + pushConstants.vertexOffset;
    vec4 worldPosition = pushConstants.modelMatrix * vec4(LoadPosition(uint(resolvedVertexIndex)), 1.0);
    gl_Position = shadowMatrix.lightViewProjectionMatrix * worldPosition;
}
