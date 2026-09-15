#include <metal_stdlib>
// 이 셰이더가 사용하는 바인딩과 입력 크기다.
#define RENDERER_MAX_DIRECTIONAL_LIGHTS 4
#define RENDERER_MAX_POINT_LIGHTS 16
#define RENDERER_MAX_SPOT_LIGHTS 16

#define RENDERER_BINDING_SHADOW_MATRIX 3
#define RENDERER_BINDING_INLINE_CONSTANTS 10

#ifndef RENDERER_ENABLE_SHADOWS
#error RENDERER_ENABLE_SHADOWS must be defined
#endif

#ifndef RENDERER_VERTEX_ENTRY
#error RENDERER_VERTEX_ENTRY must be defined
#endif

using namespace metal;

struct DrawConstants
{
    float4x4 viewProjectionMatrix;
    float4x4 modelMatrix;
    uint textureFlags;
    uint padding0;
    uint padding1;
    uint padding2;
    float4 emissiveColor;
    float4 baseColor;
    float4 materialParams;
};

#if RENDERER_ENABLE_SHADOWS
struct ShadowMatrix
{
    float4x4 lightViewProjectionMatrix[128];
    float4 atlasRect[128];
    float4 directionalViews[RENDERER_MAX_DIRECTIONAL_LIGHTS];
    float4 pointViews[RENDERER_MAX_POINT_LIGHTS];
    float4 spotViews[RENDERER_MAX_SPOT_LIGHTS];
    float4 directionalSplits[RENDERER_MAX_DIRECTIONAL_LIGHTS];
    float4x4 cameraView;
    float4 filterParams;
};
#endif

struct MeshVertex
{
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv [[attribute(2)]];
    float4 tangent [[attribute(3)]];
};

struct RasterData
{
    float4 position [[position]];
    float2 uv [[user(locn0)]];
    float3 worldPosition [[user(locn1)]];
    float3 worldNormal [[user(locn2)]];
    float4 worldTangent [[user(locn3)]];
#if RENDERER_ENABLE_SHADOWS
    float4 lightSpacePosition [[user(locn4)]];
#endif
};

// 특이 변환에서는 기존처럼 단위행렬을 법선 변환에 사용한다.
inline float3x3 MeshNormalMatrix(float4x4 matrix)
{
    float3 a = matrix[0].xyz;
    float3 b = matrix[1].xyz;
    float3 c = matrix[2].xyz;
    float det = dot(a, cross(b, c));
    if (abs(det) <= 0.00000001f) return float3x3(1.0f);
    return float3x3(cross(b, c) / det, cross(c, a) / det, cross(a, b) / det);
}

vertex RasterData RENDERER_VERTEX_ENTRY(
    MeshVertex input [[stage_in]],
#if RENDERER_ENABLE_SHADOWS
    constant ShadowMatrix& shadowMatrix [[buffer(RENDERER_BINDING_SHADOW_MATRIX)]],
#endif
    constant DrawConstants& drawConstants [[buffer(RENDERER_BINDING_INLINE_CONSTANTS)]])
{
    const float4x4 world = drawConstants.modelMatrix;
    const float4 worldPosition = world * float4(input.position, 1.0f);
    const float3x3 normalMatrix = MeshNormalMatrix(world);

    RasterData output;
    output.position = drawConstants.viewProjectionMatrix * worldPosition;
    output.uv = input.uv;
    output.worldPosition = worldPosition.xyz;
    output.worldNormal = normalize(normalMatrix * input.normal);
    const float3x3 linear(world[0].xyz, world[1].xyz, world[2].xyz);
    float3 tangent = linear * input.tangent.xyz;
    tangent = normalize(tangent - output.worldNormal * dot(output.worldNormal, tangent));
    output.worldTangent = float4(tangent, determinant(linear) < 0.0 ? -input.tangent.w : input.tangent.w);
#if RENDERER_ENABLE_SHADOWS
    output.lightSpacePosition = shadowMatrix.lightViewProjectionMatrix[0] * worldPosition;
#endif
    return output;
}
