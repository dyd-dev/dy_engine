#include <metal_stdlib>
using namespace metal;
struct Input { float2 position [[attribute(0)]]; float4 color [[attribute(1)]]; float4 transform [[attribute(2)]]; };
struct Output { float4 position [[position]]; float4 color; };
vertex Output vertexMain(Input input [[stage_in]]) {
    return {float4(input.position*input.transform.zw+input.transform.xy,0,1),input.color};
}
fragment float4 fragmentMain(Output input [[stage_in]]) { return input.color; }
