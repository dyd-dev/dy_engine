#include <metal_stdlib>
using namespace metal;
struct Constants { float4 color; float4 parameters; };
struct Input { float3 position [[attribute(0)]]; float2 uv [[attribute(1)]]; };
struct Output { float4 position [[position]]; float2 uv; };
vertex Output main0(Input input [[stage_in]], constant Constants& constants [[buffer(15)]])
{
    Output output;
    output.position = float4(input.position.xy * constants.parameters.x, constants.parameters.y, 1);
    output.uv = input.uv;
    return output;
}
