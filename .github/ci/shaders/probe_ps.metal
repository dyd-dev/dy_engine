#include <metal_stdlib>
using namespace metal;
struct Constants { float4 color; float4 parameters; };
struct Input { float4 position [[position]]; float2 uv; };
fragment float4 main0(Input input [[stage_in]], constant Constants& constants [[buffer(0)]],
                      texture2d<float> colorTexture [[texture(0)]])
{
    constexpr sampler colorSampler(coord::normalized, address::clamp_to_edge, filter::linear);
    return constants.parameters.w > 0.5 ? colorTexture.sample(colorSampler, input.uv) : constants.color;
}
