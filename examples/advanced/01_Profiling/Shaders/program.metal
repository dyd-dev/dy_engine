#include <metal_stdlib>
using namespace metal;
struct Output {float4 position [[position]];float2 uv;};
vertex Output vertexMain(uint id [[vertex_id]]){float2 uv=float2((id<<1)&2,id&2);return {float4(uv*2-1,0,1),uv};}
fragment float4 fragmentMain(Output input [[stage_in]],constant float4& value [[buffer(15)]]){
float2 z=0,c=(input.uv-float2(.65,.5))*2.6;int count=0;
for(int i=0;i<int(value.x);++i){z=float2(z.x*z.x-z.y*z.y,2*z.x*z.y)+c;if(dot(z,z)>16)break;count++;}
float t=float(count)/max(value.x,1.f);return float4(.08+t,.12+t*t,.2+sqrt(t)*.7,1);}
