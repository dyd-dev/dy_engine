#version 450
#extension GL_GOOGLE_include_directive : require

#include "Graphics/RendererShaderLayout.inc"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;
layout(location = 3) in vec4 inTangent;

layout(location = 0) out vec2 fragUv;
layout(location = 1) out vec3 fragWorldPosition;
layout(location = 2) out vec3 fragNormal;
layout(location = 3) out vec4 fragTangent;
layout(location = 4) out vec4 fragLightSpacePosition;

layout(push_constant) uniform DrawConstants {
    mat4 viewProjectionMatrix;
    mat4 modelMatrix;
    float drawMode;
    uint instanceTransformOffset;
    vec3 emissiveColor;
    float baseColorTextureIndex;
    vec4 baseColor;
    vec4 materialParams;
} pushConstants;

layout(set = 0, binding = DY_RENDERER_BINDING_SHADOW_MATRIX) uniform ShadowMatrix {
    mat4 lightViewProjectionMatrix;
} shadowMatrix;

void main() {
    vec4 worldPosition = pushConstants.modelMatrix * vec4(inPosition, 1.0);
    gl_Position = pushConstants.viewProjectionMatrix * worldPosition;
    fragUv = inUv;
    fragWorldPosition = worldPosition.xyz;
    fragNormal = normalize(mat3(pushConstants.modelMatrix) * inNormal);
    fragTangent = vec4(normalize(mat3(pushConstants.modelMatrix) * inTangent.xyz), inTangent.w);
    fragLightSpacePosition = shadowMatrix.lightViewProjectionMatrix * worldPosition;
}
