#include <metal_stdlib>
using namespace metal;
struct Output {float4 position [[position]];float2 uv;};
vertex Output vertexMain(uint id [[vertex_id]]){float2 uv=float2((id<<1)&2,id&2);return {float4(uv*2-1,0,1),uv};}
fragment float4 fragmentMain(Output input [[stage_in]],constant float4& value [[buffer(15)]]){float bar=step(abs(input.uv.x-fract(value.x*.35)),.035f);float stripe=step(.5f,fract(input.uv.y*40));return float4(mix(float3(.025,.045,.08),float3(.2+.7*stripe,.8,.3),bar),1);}
