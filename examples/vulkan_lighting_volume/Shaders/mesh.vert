#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec3 fragWorldPosition;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec2 fragUV;

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    mat4 model;
} pushConstants;

void main() {
    vec4 worldPosition = pushConstants.model * vec4(inPosition, 1.0);
    gl_Position = pushConstants.viewProj * worldPosition;
    fragWorldPosition = worldPosition.xyz;
    fragNormal = normalize(mat3(pushConstants.model) * inNormal);
    fragUV = inUV;
}
