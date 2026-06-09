// mesh_shadow_vs.hlsl - D3D12 depth-only shadow pass (writes light-space depth).
// IA vertex input (only POSITION is consumed); caster filtering happens on the CPU
// in the Graphics layer, so the shader stays trivial.

cbuffer DrawConstants : register(b0)
{
    column_major float4x4 viewProjectionMatrix;
    column_major float4x4 modelMatrix;
    float drawMode;
    uint  firstIndex;
    int   vertexOffset;
    uint  firstVertex;
    float3 emissiveColor;
    float  baseColorTextureIndex;
    float4 baseColor;
    float4 materialParams;
};

cbuffer ShadowMatrix : register(b3)
{
    column_major float4x4 lightViewProjectionMatrix;
};

float4 main(float3 position : POSITION) : SV_POSITION
{
    float4 worldPosition = mul(modelMatrix, float4(position, 1.0));
    return mul(lightViewProjectionMatrix, worldPosition);
}
