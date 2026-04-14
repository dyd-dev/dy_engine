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

fragment float4 main0(
    VSOutput input [[stage_in]],
    constant DrawConstants& gDraw [[buffer(0)]],
    array<texture2d<float>, 1024> gTextures [[texture(0)]],
    sampler gLinearSampler [[sampler(0)]])
{
    return gTextures[gDraw.baseColorTextureIndex].sample(gLinearSampler, input.uv) * gDraw.baseColor;
}
