#version 450

layout(location = 0) out vec2 fragUv;
layout(location = 1) out vec4 fragColor;

layout(push_constant) uniform DrawConstants {
    mat4 worldMatrix;
    vec4 baseColor;
    uint baseColorTextureIndex;
    vec3 padding;
} pushConstants;

void main() {
    const vec2 positions[3] = vec2[](
        vec2(0.0, 0.6),
        vec2(0.6, -0.6),
        vec2(-0.6, -0.6)
    );
    const vec2 uvs[3] = vec2[](
        vec2(0.5, 0.0),
        vec2(1.0, 1.0),
        vec2(0.0, 1.0)
    );

    vec4 localPosition = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    gl_Position = pushConstants.worldMatrix * localPosition;
    fragUv = uvs[gl_VertexIndex];
    fragColor = pushConstants.baseColor;
}
