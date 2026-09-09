#include "Graphics/ShaderInterop/StockShaderLayout.inc"
#include "Skinning.hlsl"

#define REGISTER_TOKEN_IMPL(prefix, index) prefix##index
#define REGISTER_TOKEN(prefix, index) REGISTER_TOKEN_IMPL(prefix, index)

cbuffer DrawConstants : register(
    REGISTER_TOKEN(b, RENDERER_BINDING_INLINE_CONSTANTS),
    REGISTER_TOKEN(space, RENDERER_DESCRIPTOR_SET))
{
    column_major float4x4 viewProjectionMatrix;
    column_major float4x4 modelMatrix;
    uint textureFlags;
    uint padding0;
    uint padding1;
    uint padding2;
    float4 emissiveColor;
    float4 baseColor;
    float4 materialParams;
};

cbuffer ShadowMatrix : register(
    REGISTER_TOKEN(b, RENDERER_BINDING_SHADOW_MATRIX),
    REGISTER_TOKEN(space, RENDERER_DESCRIPTOR_SET))
{
    column_major float4x4 lightViewProjectionMatrix;
};

float4 main(float3 position : TEXCOORD0, uint vertexId : SV_VertexID) : SV_POSITION
{
    float4x4 skin, skinNormal;
    LoadSkinning(vertexId, padding0, padding1, skin, skinNormal);
    return mul(lightViewProjectionMatrix, mul(modelMatrix, mul(skin, float4(position, 1.0))));
}
