#include <metal_stdlib>
using namespace metal;
struct Output {float4 position [[position]];float2 uv;};
vertex Output vertexMain(uint id [[vertex_id]]){float2 uv=float2((id<<1)&2,id&2);return {float4(uv*2-1,0,1),uv};}
fragment float4 fragmentMain(Output input [[stage_in]],constant float4& value [[buffer(15)]]){float radius=value.x<.5?.30:.35;
if((value.x<.5 || value.z>.5) && length((input.uv-float2(.5))*float2(value.y,1))>radius)discard_fragment();
float checker=fmod(floor(input.uv.x*24)+floor(input.uv.y*18),2.f);return float4(mix(float3(.1,.35,.9),float3(1,.65,.08),checker),1);}
