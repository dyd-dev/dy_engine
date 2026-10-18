#version 450
#extension GL_GOOGLE_include_directive : require

#include "ModelShaderLayout.inc"
#include "Skinning.glsl"

#ifndef RENDERER_ENABLE_SHADOWS
#error RENDERER_ENABLE_SHADOWS must be defined
#endif

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;
layout(location = 3) in vec4 inTangent;

layout(location = 0) out vec2 fragUv;
layout(location = 1) out vec3 fragWorldPosition;
layout(location = 2) out vec3 fragNormal;
layout(location = 3) out vec4 fragTangent;
#if RENDERER_ENABLE_SHADOWS
layout(location = 4) out vec4 fragLightSpacePosition;
#endif

layout(push_constant) uniform DrawConstants
{
    mat4 viewProjectionMatrix;
    mat4 modelMatrix;
    uint textureFlags;
    uint padding0;
    uint padding1;
    uint padding2;
    vec4 emissiveColor;
    vec4 baseColor;
    vec4 materialParams;
    // Model 확장이 추가한 상수이며 기본 Renderer의 padding을 사용하지 않는다.
    uint influenceOffset;
    uint paletteOffset;
    // Metal 상수 구조체의 16바이트 정렬을 포함하여 추가 영역을 16바이트로 맞춘다.
    uint modelPadding0;
    uint modelPadding1;
} drawConstants;

#if RENDERER_ENABLE_SHADOWS
layout(set = RENDERER_DESCRIPTOR_SET, binding = RENDERER_BINDING_SHADOW_MATRIX) uniform ShadowMatrix
{
    #define DY_SHADOW_MATRIX mat4
#define DY_SHADOW_FLOAT4 vec4
#include "ModelShadowFields.inc"
#undef DY_SHADOW_MATRIX
#undef DY_SHADOW_FLOAT4
} shadowMatrix;
#endif

void main()
{
    mat4 skin, skinNormal;
    LoadSkinning(gl_VertexIndex, drawConstants.influenceOffset, drawConstants.paletteOffset, skin, skinNormal);
    mat4 world = drawConstants.modelMatrix * skin;
    mat4 modelNormal = SkinNormalMatrix(drawConstants.modelMatrix, SkinIdentity());
    // Resolve skin fallbacks before the model transform, as CPU/compute do.
    vec3 skinnedNormal = SkinNormalizeOr(mat3(skinNormal) * inNormal, vec3(0,0,1));
    vec3 skinnedTangent = mat3(skin) * inTangent.xyz;
    skinnedTangent = SkinNormalizeOr(skinnedTangent - skinnedNormal * dot(skinnedNormal, skinnedTangent), SkinFallbackTangent(skinnedNormal));
    vec4 worldPosition = world * vec4(inPosition, 1.0);
    gl_Position = drawConstants.viewProjectionMatrix * worldPosition;
    fragUv = inUv;
    fragWorldPosition = worldPosition.xyz;
    vec3 fallbackNormal = SkinNormalizeOr(SkinColumn(modelNormal, 2).xyz, vec3(0,0,1));
    fragNormal = SkinNormalizeOr(mat3(modelNormal) * skinnedNormal, fallbackNormal);
    vec3 tangent = mat3(drawConstants.modelMatrix) * skinnedTangent;
    tangent = SkinNormalizeOr(tangent - fragNormal * dot(fragNormal, tangent), SkinFallbackTangent(fragNormal));
    float orientation = (determinant(mat3(skin)) < 0.0 ? -1.0 : 1.0) *
        (determinant(mat3(drawConstants.modelMatrix)) < 0.0 ? -1.0 : 1.0);
    fragTangent = vec4(tangent, orientation * inTangent.w);
#if RENDERER_ENABLE_SHADOWS
    fragLightSpacePosition = shadowMatrix.lightViewProjectionMatrix[0] * worldPosition;
#endif
}
