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

struct ShadowMatrix
{
// 정적 예제의 방향광 1개·점광원 6면·스폿 광원 1개를 같은 깊이 아틀라스에 저장한다.
float4x4 lightViewProjectionMatrix[8];
float4 atlasRect[8];
float4 directionalViews[1];
float4 pointViews[1];
float4 spotViews[1];

};

struct ShadowVertex
{
    float3 position [[attribute(0)]];
};

vertex float4 shadowVertexShader(
    ShadowVertex input [[stage_in]],
    constant DrawConstants& drawConstants [[buffer(10)]],
    constant ShadowMatrix& shadowMatrix [[buffer(3)]]) [[position]]
{
    return shadowMatrix.lightViewProjectionMatrix[drawConstants.shadowViewIndex] * drawConstants.modelMatrix * float4(input.position, 1.0f);
}
