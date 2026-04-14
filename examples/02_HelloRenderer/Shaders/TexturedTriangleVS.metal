#include <metal_stdlib>
using namespace metal;

struct DrawConstants
{
    float4x4 worldMatrix;
    float4 baseColor;
    uint baseColorTextureIndex;
    float3 padding;
};

struct VSOutput
{
    float4 position [[position]];
    float2 uv;
};

vertex VSOutput main0(uint vertexID [[vertex_id]], constant DrawConstants& gDraw [[buffer(0)]])
{
    const float2 positions[3] =
    {
        float2(0.0, 0.6),
        float2(0.6, -0.6),
        float2(-0.6, -0.6)
    };

    const float2 uvs[3] =
    {
        float2(0.5, 0.0),
        float2(1.0, 1.0),
        float2(0.0, 1.0)
    };

    VSOutput output;
    output.position = gDraw.worldMatrix * float4(positions[vertexID], 0.0, 1.0);
    output.uv = uvs[vertexID];
    return output;
}
