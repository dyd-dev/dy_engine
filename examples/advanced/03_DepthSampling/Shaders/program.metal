#include <metal_stdlib>
using namespace metal;
struct Output {float4 position [[position]];float2 uv;};
vertex Output vertexMain(uint id [[vertex_id]]){float2 uv=float2((id<<1)&2,id&2);return {float4(uv*2-1,0,1),uv};}
struct DepthOutput {float depth [[depth(any)]];};
fragment DepthOutput producerMain(Output input [[stage_in]],constant float4& value [[buffer(15)]]){
float n=value.z,f=value.w;float distance=mix(n+1,f*.9,input.uv.x);if(length(input.uv-float2(.3,.5))<.2)distance=n+2;
return {(f-f*n/distance)/(f-n)};}
fragment float4 fragmentMain(Output input [[stage_in]],depth2d<float> source [[texture(0)]],sampler pointSampler [[sampler(1)]],constant float4& value [[buffer(15)]]){
float2 uv=input.uv;bool split=value.x<.5;float2 coord=float2(split?fract(uv.x*2):uv.x,uv.y);float d=source.sample(pointSampler,coord);
float n=value.z,f=value.w,linearDepth=n*f/max(f-d*(f-n),.000001f);bool linearView=value.x>1.5 || (split && uv.x>.5);
float intensity=linearView?linearDepth/f:d;if(split && abs(uv.x-.5)<.003)return float4(.8,.2,.1,1);return float4(float3(intensity),1);}
