#include "ModelShaderLayout.inc"
#include "Skinning.hlsl"

#ifndef RENDERER_ENABLE_SHADOWS
#error RENDERER_ENABLE_SHADOWS must be defined
#endif

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
    // Model 확장이 추가한 상수이며 기본 Renderer의 padding을 사용하지 않는다.
    uint influenceOffset;
    uint paletteOffset;
    // Metal 상수 구조체의 16바이트 정렬을 포함하여 추가 영역을 16바이트로 맞춘다.
    uint modelPadding0;
    uint modelPadding1;
};

#if RENDERER_ENABLE_SHADOWS
cbuffer ShadowMatrix : register(
    REGISTER_TOKEN(b, RENDERER_BINDING_SHADOW_MATRIX),
    REGISTER_TOKEN(space, RENDERER_DESCRIPTOR_SET))
{
    #define DY_SHADOW_MATRIX float4x4
#define DY_SHADOW_FLOAT4 float4
#include "ModelShadowFields.inc"
#undef DY_SHADOW_MATRIX
#undef DY_SHADOW_FLOAT4
};
#endif

struct VSInput
{
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 tangent : TEXCOORD3;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float3 worldPosition : TEXCOORD1;
    float3 worldNormal : TEXCOORD2;
    float4 worldTangent : TEXCOORD3;
#if RENDERER_ENABLE_SHADOWS
    float4 lightSpacePosition : TEXCOORD4;
#endif
};

VSOutput main(VSInput input, uint vertexId : SV_VertexID)
{
    float4x4 skin, skinNormal;
    LoadSkinning(vertexId, influenceOffset, paletteOffset, skin, skinNormal);
    const float4x4 world = mul(modelMatrix, skin);
    const float4 worldPosition = mul(world, float4(input.position, 1.0));
    const float3x3 normalMatrix = (float3x3)SkinNormalMatrix(world, skinNormal);

    VSOutput output;
    output.position = mul(viewProjectionMatrix, worldPosition);
    output.uv = input.uv;
    output.worldPosition = worldPosition.xyz;
    output.worldNormal = normalize(mul(normalMatrix, input.normal));
    float3 tangent = mul((float3x3)world, input.tangent.xyz);
    tangent = normalize(tangent - output.worldNormal * dot(output.worldNormal, tangent));
    output.worldTangent = float4(tangent, determinant((float3x3)world) < 0.0 ? -input.tangent.w : input.tangent.w);
#if RENDERER_ENABLE_SHADOWS
    output.lightSpacePosition = mul(lightViewProjectionMatrix[0], worldPosition);
#endif
    return output;
}
