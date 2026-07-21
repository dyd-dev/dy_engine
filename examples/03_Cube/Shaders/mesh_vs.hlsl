// mesh_vs.hlsl - D3D12 IA vertex input + Renderer DrawConstants.

cbuffer DrawConstants : register(b0)
{
    column_major float4x4 viewProjectionMatrix;
    column_major float4x4 modelMatrix;
    float drawMode;
    uint  instanceTransformOffset;
    float3 emissiveColor;
    float  baseColorTextureIndex;
    float4 baseColor;
    float4 materialParams;
};

cbuffer ShadowMatrix : register(b3)
{
    column_major float4x4 lightViewProjectionMatrix;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
    float4 tangent  : TANGENT;
};

struct VSOutput
{
    float4 position           : SV_POSITION;
    float2 uv                 : TEXCOORD0;
    float3 worldPosition      : TEXCOORD1;
    float3 worldNormal        : TEXCOORD2;
    float4 worldTangent       : TEXCOORD3;
    float4 lightSpacePosition : TEXCOORD4;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    float4 worldPosition = mul(modelMatrix, float4(input.position, 1.0));
    float3x3 worldMatrix3x3 = (float3x3)modelMatrix;

    output.position = mul(viewProjectionMatrix, worldPosition);
    output.uv = input.uv;
    output.worldPosition = worldPosition.xyz;
    output.worldNormal = normalize(mul(worldMatrix3x3, input.normal));
    output.worldTangent = float4(normalize(mul(worldMatrix3x3, input.tangent.xyz)), input.tangent.w);
    output.lightSpacePosition = mul(lightViewProjectionMatrix, worldPosition);
    return output;
}
