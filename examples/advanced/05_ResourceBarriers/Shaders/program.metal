#include <metal_stdlib>
using namespace metal;
struct Output {float4 position [[position]];float2 uv;};
vertex Output vertexMain(uint id [[vertex_id]]){float2 uv=float2((id<<1)&2,id&2);return {float4(uv*2-1,0,1),uv};}
fragment float4 producerMain(Output input [[stage_in]],constant float4& value [[buffer(15)]]){if(value.x>.5 && input.uv.x<.5)discard_fragment();return float4(value.x>.5?float3(.15,.9,.35):float3(.95,.2,.1),1);}
fragment float4 fragmentMain(Output input [[stage_in]],texture2d<float> source [[texture(0)]],sampler pointSampler [[sampler(1)]]){return source.sample(pointSampler,input.uv);}
