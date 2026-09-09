#include <metal_stdlib>
#include "Graphics/ShaderInterop/StockShaderLayout.inc"

using namespace metal;
#include "Skinning.metal"

struct DrawConstants
{
    float4x4 viewProjectionMatrix;
    float4x4 modelMatrix;
    uint textureFlags;
    uint padding0;
    uint padding1;
    uint padding2;
    float4 emissiveColor;
    float4 baseColor;
    float4 materialParams;
};

struct ShadowMatrix
{
    float4x4 lightViewProjectionMatrix;
};

struct ShadowVertex
{
    float3 position [[attribute(0)]];
};

vertex float4 shadowVertexShader(
    ShadowVertex input [[stage_in]],
    uint vertexId [[vertex_id]],
    const device SkinInfluence* skinInfluences [[buffer(RENDERER_BINDING_SKIN_INFLUENCES)]],
    const device SkinJointMatrices* skinPalette [[buffer(RENDERER_BINDING_SKIN_PALETTE)]],
    constant DrawConstants& drawConstants [[buffer(RENDERER_BINDING_INLINE_CONSTANTS)]],
    constant ShadowMatrix& shadowMatrix [[buffer(RENDERER_BINDING_SHADOW_MATRIX)]]) [[position]]
{
    float4x4 skin, skinNormal;
    LoadSkinning(skinInfluences, skinPalette, vertexId, drawConstants.padding0, drawConstants.padding1, skin, skinNormal);
    return shadowMatrix.lightViewProjectionMatrix * drawConstants.modelMatrix * skin * float4(input.position, 1.0f);
}
