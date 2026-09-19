cbuffer DrawConstants : register(b10, space0)
{
    column_major float4x4 viewProjectionMatrix;
    column_major float4x4 modelMatrix;
    float4 baseColor;
    float drawMetallic;
    float drawRoughness;
};

struct VertexInput
{
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
};
struct RasterData
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
};

// HLSL 행렬의 열벡터로 역전치 행렬을 구한다.
float3x3 MeshNormalMatrix(float4x4 matrix)
{
    float3 a = float3(matrix[0][0], matrix[1][0], matrix[2][0]);
    float3 b = float3(matrix[0][1], matrix[1][1], matrix[2][1]);
    float3 c = float3(matrix[0][2], matrix[1][2], matrix[2][2]);
    float determinant = dot(a, cross(b, c));
    if(abs(determinant) <= 0.00000001) return float3x3(1,0,0, 0,1,0, 0,0,1);
    return transpose(float3x3(cross(b, c) / determinant, cross(c, a) / determinant, cross(a, b) / determinant));
}

RasterData main(VertexInput input)
{
    float4 world = mul(modelMatrix, float4(input.position, 1.0));
    RasterData output;
    output.position = mul(viewProjectionMatrix, world);
    output.worldPosition = world.xyz;
    output.worldNormal = normalize(mul(MeshNormalMatrix(modelMatrix), input.normal));
    return output;
}
