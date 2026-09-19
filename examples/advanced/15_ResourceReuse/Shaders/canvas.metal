#include <metal_stdlib>
using namespace metal;
struct Input
{
    float2 position [[attribute(0)]];
    float4 instanceTransform [[attribute(1)]];
    float4 instanceColor [[attribute(2)]];
};
struct Output { float4 position [[position]]; float4 color; };
vertex Output canvasVertex(Input input [[stage_in]])
{
    return {float4(input.position * input.instanceTransform.zw + input.instanceTransform.xy, 0, 1),
        input.instanceColor};
}
fragment float4 canvasFragment(Output input [[stage_in]]) { return input.color; }
