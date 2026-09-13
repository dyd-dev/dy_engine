#include <metal_stdlib>
using namespace metal;
struct DrawConstants
{
    float4x4 viewProjectionMatrix;
    float4x4 modelMatrix;
    float4 baseColor;
    float metallic;
    float roughness;
};

struct VertexInput
{
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
};
struct RasterData
{
    float4 position [[position]];
    float3 worldPosition [[user(locn0)]];
    float3 worldNormal [[user(locn1)]];
};

// 비균일 배율을 포함한 법선 변환이며 특이 행렬에서는 단위행렬을 사용한다.
inline float3x3 MeshNormalMatrix(float4x4 matrix)
{
    float3 a = matrix[0].xyz, b = matrix[1].xyz, c = matrix[2].xyz;
    float determinant = dot(a, cross(b, c));
    if(abs(determinant) <= 0.00000001f) return float3x3(1.0f);
    return float3x3(cross(b, c) / determinant, cross(c, a) / determinant, cross(a, b) / determinant);
}

vertex RasterData vertexMain(VertexInput input [[stage_in]],
    constant DrawConstants& draw [[buffer(10)]])
{
    float4 world = draw.modelMatrix * float4(input.position, 1.0f);
    RasterData output;
    output.position = draw.viewProjectionMatrix * world;
    output.worldPosition = world.xyz;
    output.worldNormal = normalize(MeshNormalMatrix(draw.modelMatrix) * input.normal);
    return output;
}
