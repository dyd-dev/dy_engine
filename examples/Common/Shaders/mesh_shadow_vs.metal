#include <metal_stdlib>
using namespace metal;

struct DrawConstants
{
    float4x4 viewProjectionMatrix;
    float4x4 modelMatrix;
    float drawMode;
    uint firstIndex;
    int vertexOffset;
    uint firstVertex;
    float4 emissiveColor;
    float4 baseColor;
    float4 materialParams;
    float4 textureIndices;
};

struct ShadowConstants
{
    float4x4 lightViewProjectionMatrices[6];
    float4 cascadeSplits;
    float4 shadowInfo;
    float4 pcssParams;
    float4x4 cameraViewMatrix;
};

vertex float4 main0(
    uint vertexId [[vertex_id]],
    constant DrawConstants& draw [[buffer(0)]],
    constant ShadowConstants& shadow [[buffer(3)]],
    device const float* vertices [[buffer(4)]],
    device const uint* indices [[buffer(5)]])
{
    constexpr uint floatsPerVertex = 12u;
    const int resolvedIndex = int(indices[draw.firstIndex + vertexId]) + draw.vertexOffset;
    const uint base = uint(resolvedIndex) * floatsPerVertex;
    const float3 localPosition = float3(vertices[base], vertices[base + 1u], vertices[base + 2u]);
    const uint viewIndex = min(uint(max(draw.emissiveColor.w, 0.0f) + 0.5f), 5u);
    return shadow.lightViewProjectionMatrices[viewIndex]
        * draw.modelMatrix * float4(localPosition, 1.0f);
}
