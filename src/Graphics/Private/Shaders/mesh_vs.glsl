#version 450
#extension GL_GOOGLE_include_directive : require

#include "Graphics/ShaderInterop/StockShaderLayout.inc"
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
} drawConstants;

#if RENDERER_ENABLE_SHADOWS
layout(set = RENDERER_DESCRIPTOR_SET, binding = RENDERER_BINDING_SHADOW_MATRIX) uniform ShadowMatrix
{
    mat4 lightViewProjectionMatrix;
} shadowMatrix;
#endif

void main()
{
    mat4 skin, skinNormal;
    LoadSkinning(gl_VertexIndex, drawConstants.padding0, drawConstants.padding1, skin, skinNormal);
    mat4 world = drawConstants.modelMatrix * skin;
    mat4 normalMatrix = SkinNormalMatrix(world, skinNormal);
    vec4 worldPosition = world * vec4(inPosition, 1.0);
    gl_Position = drawConstants.viewProjectionMatrix * worldPosition;
    fragUv = inUv;
    fragWorldPosition = worldPosition.xyz;
    fragNormal = normalize(mat3(normalMatrix) * inNormal);
    vec3 tangent = mat3(world) * inTangent.xyz;
    tangent = normalize(tangent - fragNormal * dot(fragNormal, tangent));
    fragTangent = vec4(tangent, determinant(mat3(world)) < 0.0 ? -inTangent.w : inTangent.w);
#if RENDERER_ENABLE_SHADOWS
    fragLightSpacePosition = shadowMatrix.lightViewProjectionMatrix * worldPosition;
#endif
}
