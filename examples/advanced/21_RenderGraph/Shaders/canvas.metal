#include <metal_stdlib>
using namespace metal;
struct Input { float2 position [[attribute(0)]]; };
struct Constants { float4 transform; float4 tint; };
struct Output { float4 position [[position]]; float4 color; };
vertex Output canvasVertex(Input input [[stage_in]], constant Constants& constants [[buffer(15)]])
{
    return {float4(input.position * constants.transform.xy + constants.transform.zw, 0, 1), constants.tint};
}
fragment float4 canvasFragment(Output input [[stage_in]]) { return input.color; }
