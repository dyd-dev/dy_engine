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

layout(std430, set = 0, binding = DY_RENDERER_BINDING_BINDLESS_TRANSFORM_STORAGE) readonly buffer InstanceTransformStorage {
    mat4 modelMatrices[];
} instanceTransforms;

layout(set = 0, binding = DY_RENDERER_BINDING_SHADOW_MATRIX) uniform ShadowMatrix {
    mat4 lightViewProjectionMatrix;
} shadowMatrix;

void main() {
    uint instanceBase = pushConstants.instanceTransformOffset;
    mat4 resolvedModelMatrix = pushConstants.modelMatrix;
    if (instanceBase != 0u) {
        resolvedModelMatrix = instanceTransforms.modelMatrices[instanceBase - 1u + gl_InstanceIndex];
    }
    vec4 worldPosition = resolvedModelMatrix * vec4(inPosition, 1.0);
    gl_Position = pushConstants.viewProjectionMatrix * worldPosition;
    fragUv = inUv;
    fragWorldPosition = worldPosition.xyz;
    fragNormal = normalize(mat3(resolvedModelMatrix) * inNormal);
    fragTangent = vec4(normalize(mat3(resolvedModelMatrix) * inTangent.xyz), inTangent.w);
    fragLightSpacePosition = shadowMatrix.lightViewProjectionMatrix * worldPosition;
}
