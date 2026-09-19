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

struct VSInput
{
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD1;
    float3 worldNormal : TEXCOORD2;
};

// 특이 변환에서는 기존처럼 단위행렬을 법선 변환에 사용한다.
float3x3 MeshNormalMatrix(float4x4 matrix)
{
    float3 a = float3(matrix[0][0], matrix[1][0], matrix[2][0]);
    float3 b = float3(matrix[0][1], matrix[1][1], matrix[2][1]);
    float3 c = float3(matrix[0][2], matrix[1][2], matrix[2][2]);
    float det = dot(a, cross(b, c));
    if (abs(det) <= 0.00000001) return float3x3(1,0,0, 0,1,0, 0,0,1);
    return transpose(float3x3(cross(b, c) / det, cross(c, a) / det, cross(a, b) / det));
}

VSOutput main(VSInput input)
{
    const float4x4 world = modelMatrix;
    const float4 worldPosition = mul(world, float4(input.position, 1.0));
    const float3x3 normalMatrix = MeshNormalMatrix(world);

    VSOutput output;
    output.position = mul(viewProjectionMatrix, worldPosition);
    output.worldPosition = worldPosition.xyz;
    output.worldNormal = normalize(mul(normalMatrix, input.normal));
    return output;
}
