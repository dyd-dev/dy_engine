cbuffer DrawConstants : register(
    b10,
    space0)
{
    column_major float4x4 viewProjectionMatrix;
    column_major float4x4 modelMatrix;
    float4 baseColor;
    float drawMetallic;
    float drawRoughness;
    uint drawReceiveShadow;
    uint shadowViewIndex;
};

cbuffer ShadowMatrix : register(
    b3,
    space0)
{
// 정적 예제의 방향광 1개·점광원 6면·스폿 광원 1개를 같은 깊이 아틀라스에 저장한다.
float4x4 lightViewProjectionMatrix[8];
float4 atlasRect[8];
float4 directionalViews[1];
float4 pointViews[1];
float4 spotViews[1];

};

float4 main(float3 position : TEXCOORD0) : SV_POSITION
{
    return mul(lightViewProjectionMatrix[shadowViewIndex], mul(modelMatrix, float4(position, 1.0)));
}
