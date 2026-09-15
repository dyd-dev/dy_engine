#include <metal_stdlib>
using namespace metal;
struct Input { float2 position [[attribute(0)]]; float4 color [[attribute(1)]]; };
struct Output { float4 position [[position]]; float4 color; };

vertex Output canvasVertex(Input input [[stage_in]],constant float4& transform [[buffer(15)]])
{
    return {float4(input.position*transform.xy+transform.zw,0,1),input.color};
}

fragment float4 canvasFragment(Output input [[stage_in]])
{
    return input.color;
}
