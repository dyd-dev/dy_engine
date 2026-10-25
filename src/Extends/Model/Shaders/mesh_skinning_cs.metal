#include <metal_stdlib>
#include "ModelShaderLayout.inc"

using namespace metal;
#include "Skinning.metal"

struct SkinningDispatch
{
    uint vertexCount;
    uint influenceOffset;
    uint paletteOffset;
    uint reserved;
};

// ModelRenderer supplies a 64 x 1 x 1 threadgroup through the RHI pipeline.
kernel void modelSkinningCompute(
    uint3 threadId [[thread_position_in_grid]],
    constant SkinningDispatch& dispatchData [[buffer(RENDERER_BINDING_INLINE_CONSTANTS)]],
    const device SkinInfluence* skinInfluences [[buffer(RENDERER_BINDING_SKIN_INFLUENCES)]],
    const device SkinJointMatrices* skinPalette [[buffer(RENDERER_BINDING_SKIN_PALETTE)]],
    const device float* sourceVertices [[buffer(14)]],
    device float* skinnedVertices [[buffer(15)]])
{
    const uint vertexId = threadId.x;
    if(vertexId >= dispatchData.vertexCount) return;

    // The renderer vertex is exactly 16 floats (64 bytes). Raw scalar access
    // avoids Metal float3's 16-byte alignment changing its storage layout.
    const uint base = vertexId * 16u;
    float4x4 skin, normalSkin;
    LoadSkinning(skinInfluences, skinPalette, vertexId, dispatchData.influenceOffset,
        dispatchData.paletteOffset, skin, normalSkin);

    float3 position(sourceVertices[base], sourceVertices[base + 1u], sourceVertices[base + 2u]);
    float3 normal(sourceVertices[base + 3u], sourceVertices[base + 4u], sourceVertices[base + 5u]);
    float3 tangent(sourceVertices[base + 8u], sourceVertices[base + 9u], sourceVertices[base + 10u]);
    position = (skin * float4(position, 1.0f)).xyz;
    normal = SkinNormalizeOr((normalSkin * float4(normal, 0.0f)).xyz, float3(0, 0, 1));
    tangent = (skin * float4(tangent, 0.0f)).xyz;
    tangent = SkinNormalizeOr(tangent - normal * dot(normal, tangent), SkinFallbackTangent(normal));

    for(uint component = 0u; component < 3u; ++component)
    {
        skinnedVertices[base + component] = position[component];
        skinnedVertices[base + 3u + component] = normal[component];
        skinnedVertices[base + 8u + component] = tangent[component];
    }
    skinnedVertices[base + 6u] = sourceVertices[base + 6u];
    skinnedVertices[base + 7u] = sourceVertices[base + 7u];
    const float3x3 linear(skin[0].xyz, skin[1].xyz, skin[2].xyz);
    skinnedVertices[base + 11u] = sourceVertices[base + 11u] *
        (determinant(linear) < 0.0f ? -1.0f : 1.0f);
    for(uint component=12u;component<16u;++component)
        skinnedVertices[base+component]=sourceVertices[base+component];
}
