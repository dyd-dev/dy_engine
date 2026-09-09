#include <metal_stdlib>
#include "Graphics/ShaderInterop/StockShaderLayout.inc"

#ifndef RENDERER_ENABLE_SHADOWS
#error RENDERER_ENABLE_SHADOWS must be defined
#endif

#ifndef RENDERER_VERTEX_ENTRY
#error RENDERER_VERTEX_ENTRY must be defined
#endif

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

#if RENDERER_ENABLE_SHADOWS
struct ShadowMatrix
{
    float4x4 lightViewProjectionMatrix;
};
#endif

struct MeshVertex
{
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv [[attribute(2)]];
    float4 tangent [[attribute(3)]];
};

struct RasterData
{
    float4 position [[position]];
    float2 uv [[user(locn0)]];
    float3 worldPosition [[user(locn1)]];
    float3 worldNormal [[user(locn2)]];
    float4 worldTangent [[user(locn3)]];
#if RENDERER_ENABLE_SHADOWS
    float4 lightSpacePosition [[user(locn4)]];
#endif
};

vertex RasterData RENDERER_VERTEX_ENTRY(
    MeshVertex input [[stage_in]],
    uint vertexId [[vertex_id]],
    const device SkinInfluence* skinInfluences [[buffer(RENDERER_BINDING_SKIN_INFLUENCES)]],
    const device SkinJointMatrices* skinPalette [[buffer(RENDERER_BINDING_SKIN_PALETTE)]],
#if RENDERER_ENABLE_SHADOWS
    constant ShadowMatrix& shadowMatrix [[buffer(RENDERER_BINDING_SHADOW_MATRIX)]],
#endif
    constant DrawConstants& drawConstants [[buffer(RENDERER_BINDING_INLINE_CONSTANTS)]])
{
    float4x4 skin, skinNormal;
    LoadSkinning(skinInfluences, skinPalette, vertexId, drawConstants.padding0, drawConstants.padding1, skin, skinNormal);
    const float4x4 world = drawConstants.modelMatrix * skin;
    const float4 worldPosition = world * float4(input.position, 1.0f);
    const float4x4 normal = SkinNormalMatrix(world, skinNormal);
    const float3x3 normalMatrix = float3x3(
        normal[0].xyz,
        normal[1].xyz,
        normal[2].xyz);

    RasterData output;
    output.position = drawConstants.viewProjectionMatrix * worldPosition;
    output.uv = input.uv;
    output.worldPosition = worldPosition.xyz;
    output.worldNormal = normalize(normalMatrix * input.normal);
    const float3x3 linear(world[0].xyz, world[1].xyz, world[2].xyz);
    float3 tangent = linear * input.tangent.xyz;
    tangent = normalize(tangent - output.worldNormal * dot(output.worldNormal, tangent));
    output.worldTangent = float4(tangent, determinant(linear) < 0.0 ? -input.tangent.w : input.tangent.w);
#if RENDERER_ENABLE_SHADOWS
    output.lightSpacePosition = shadowMatrix.lightViewProjectionMatrix * worldPosition;
#endif
    return output;
}
