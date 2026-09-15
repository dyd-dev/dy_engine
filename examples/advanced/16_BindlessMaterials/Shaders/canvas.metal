#include <metal_stdlib>
using namespace metal;
struct Input { float2 position [[attribute(0)]]; float2 uv [[attribute(1)]]; };
struct Constants { float4 transform; float4 parameters; };
struct Output { float4 position [[position]]; float2 uv; };
vertex Output canvasVertex(Input input [[stage_in]], constant Constants& constants [[buffer(15)]])
{
    return {float4(input.position * constants.transform.xy + constants.transform.zw, 0, 1), input.uv};
}
fragment float4 canvasFragment(Output input [[stage_in]],
    texture2d<float> materialTexture [[texture(0)]], sampler materialSampler [[sampler(4)]])
{
    return materialTexture.sample(materialSampler, input.uv);
}
fragment float4 arrayFragment(Output input [[stage_in]],
    array<texture2d<float>, 4> materialTextures [[texture(0)]], sampler materialSampler [[sampler(4)]],
    constant Constants& constants [[buffer(15)]])
{
    return materialTextures[uint(constants.parameters.x)].sample(materialSampler, input.uv);
}
