#include <metal_stdlib>
using namespace metal;
struct Input { float2 position [[attribute(0)]]; float4 color [[attribute(1)]]; };
struct Output { float4 position [[position]]; float4 color; };
vertex Output vertexMain(Input input [[stage_in]],constant float4& transform [[buffer(15)]]) {
    return {float4(input.position*transform.zw+transform.xy,0,1),input.color};
}
fragment float4 fragmentMain(Output input [[stage_in]],constant float4& tint [[buffer(0)]]) { return input.color*tint; }
