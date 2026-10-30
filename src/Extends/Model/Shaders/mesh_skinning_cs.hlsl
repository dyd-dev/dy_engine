#include "ModelShaderLayout.inc"
#include "Skinning.hlsl"

// The RHI storage views use ModelVertex's 64-byte stride. Keep the same sixteen
// floats as the vertex and GLSL compute paths: position, normal, UV, tangent, color.
struct ModelVertex
{
    float3 position;
    float3 normal;
    float2 uv;
    float4 tangent;
    float4 color;
};

StructuredBuffer<ModelVertex> sourceVertices : register(t14, space0);
RWStructuredBuffer<ModelVertex> skinnedVertices : register(u15, space0);

#define REGISTER_TOKEN_IMPL(prefix, index) prefix##index
#define REGISTER_TOKEN(prefix, index) REGISTER_TOKEN_IMPL(prefix, index)

cbuffer SkinningDispatch : register(
    REGISTER_TOKEN(b, RENDERER_BINDING_INLINE_CONSTANTS),
    REGISTER_TOKEN(space, RENDERER_DESCRIPTOR_SET))
{
    uint vertexCount;
    uint influenceOffset;
    uint paletteOffset;
    uint reserved;
};

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint vertex = dispatchThreadId.x;
    if(vertex >= vertexCount) return;

    float4x4 skin, normalSkin;
    LoadSkinning(vertex, influenceOffset, paletteOffset, skin, normalSkin);

    ModelVertex result = sourceVertices[vertex];
    result.position = mul(skin, float4(result.position, 1)).xyz;
    result.normal = SkinNormalizeOr(mul(normalSkin, float4(result.normal, 0)).xyz, float3(0, 0, 1));
    float3 tangent = mul(skin, float4(result.tangent.xyz, 0)).xyz;
    result.tangent.xyz = SkinNormalizeOr(tangent - result.normal * dot(result.normal, tangent),
        SkinFallbackTangent(result.normal));
    result.tangent.w *= determinant((float3x3)skin) < 0 ? -1.0 : 1.0;
    // UV is copied unchanged; tangent.w retains the source handedness and only
    // gains the orientation sign introduced by skinning.
    skinnedVertices[vertex] = result;
}
