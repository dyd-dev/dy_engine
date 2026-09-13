#version 450

// 이 셰이더가 사용하는 바인딩과 입력 크기다.
#define RENDERER_DESCRIPTOR_SET 0
#define RENDERER_MAX_DIRECTIONAL_LIGHTS 4
#define RENDERER_MAX_POINT_LIGHTS 16
#define RENDERER_MAX_SPOT_LIGHTS 16

#define RENDERER_BINDING_SHADOW_MATRIX 3

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
    mat4 lightViewProjectionMatrix[128];
    vec4 atlasRect[128];
    vec4 directionalViews[RENDERER_MAX_DIRECTIONAL_LIGHTS];
    vec4 pointViews[RENDERER_MAX_POINT_LIGHTS];
    vec4 spotViews[RENDERER_MAX_SPOT_LIGHTS];
    vec4 directionalSplits[RENDERER_MAX_DIRECTIONAL_LIGHTS];
    mat4 cameraView;
    vec4 filterParams;
} shadowMatrix;
#endif

// 특이 변환에서는 기존처럼 단위행렬을 법선 변환에 사용한다.
mat3 MeshNormalMatrix(mat4 matrix)
{
    vec3 a = matrix[0].xyz;
    vec3 b = matrix[1].xyz;
    vec3 c = matrix[2].xyz;
    float det = dot(a, cross(b, c));
    if (abs(det) <= 0.00000001) return mat3(1.0);
    return mat3(cross(b, c) / det, cross(c, a) / det, cross(a, b) / det);
}

void main()
{
    mat4 world = drawConstants.modelMatrix;
    mat3 normalMatrix = MeshNormalMatrix(world);
    vec4 worldPosition = world * vec4(inPosition, 1.0);
    gl_Position = drawConstants.viewProjectionMatrix * worldPosition;
    fragUv = inUv;
    fragWorldPosition = worldPosition.xyz;
    fragNormal = normalize(mat3(normalMatrix) * inNormal);
    vec3 tangent = mat3(world) * inTangent.xyz;
    tangent = normalize(tangent - fragNormal * dot(fragNormal, tangent));
    fragTangent = vec4(tangent, determinant(mat3(world)) < 0.0 ? -inTangent.w : inTangent.w);
#if RENDERER_ENABLE_SHADOWS
    fragLightSpacePosition = shadowMatrix.lightViewProjectionMatrix[0] * worldPosition;
#endif
}
