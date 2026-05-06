#include <metal_stdlib>
using namespace metal;

struct DrawConstants
{
    float4x4 worldMatrix;
    float4 baseColor;
    uint baseColorTextureIndex;
    float padding0;
    float padding1;
    float padding2;
};

struct VSOutput
{
    float4 position [[position]];
    float2 uv;
};

fragment float4 main0(
    VSOutput input [[stage_in]],
    constant DrawConstants& gDraw [[buffer(0)]],
    texture2d<float> gTexture [[texture(0)]])
{
    constexpr sampler s(filter::linear);
    return gTexture.sample(s, input.uv) * gDraw.baseColor;
}
