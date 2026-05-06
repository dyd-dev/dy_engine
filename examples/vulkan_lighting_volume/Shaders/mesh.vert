#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec3 fragWorldPosition;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec2 fragUV;
layout(location = 3) out float fragDrawMode;
layout(location = 4) out vec4 fragLightSpacePos;

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    mat4 model;
} pushConstants;

// Shadow Map 변환용 Light-Space ViewProjection.
// VulkanDevice가 매 프레임 SetShadowLightMatrix로 갱신.
layout(set = 0, binding = 3) uniform ShadowMatrix {
    mat4 lightViewProj;
} shadowMatrix;

void main() {
    mat4 model = pushConstants.model;
    fragDrawMode = model[3][3];
    model[3][3] = 1.0;

    vec4 worldPosition = model * vec4(inPosition, 1.0);
    gl_Position = pushConstants.viewProj * worldPosition;
    fragWorldPosition = worldPosition.xyz;
    fragNormal = normalize(mat3(model) * inNormal);
    fragUV = inUV;

    // FS에서 ShadowMap UV/depth로 변환할 수 있게 light space에서의 위치를 함께 보낸다.
    fragLightSpacePos = shadowMatrix.lightViewProj * worldPosition;
}
