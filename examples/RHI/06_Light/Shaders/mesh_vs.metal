#include <metal_stdlib>

using namespace metal;

struct DrawConstants
{
    float4x4 viewProjectionMatrix;
    float4x4 modelMatrix;
    float4 baseColor;
    float metallic;
    float roughness;
    uint receiveShadow;
    uint shadowViewIndex;
};

struct MeshVertex
{
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
};

struct RasterData
{
    float4 position [[position]];
    float3 worldPosition [[user(locn1)]];
    float3 worldNormal [[user(locn2)]];
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

vertex RasterData vertexMain(
    MeshVertex input [[stage_in]],
    constant DrawConstants& drawConstants [[buffer(10)]])
{
    const float4x4 world = drawConstants.modelMatrix;
    const float4 worldPosition = world * float4(input.position, 1.0f);
    const float3x3 normalMatrix = MeshNormalMatrix(world);

    RasterData output;
    output.position = drawConstants.viewProjectionMatrix * worldPosition;
    output.worldPosition = worldPosition.xyz;
    output.worldNormal = normalize(normalMatrix * input.normal);
    return output;
}
