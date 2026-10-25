#include <metal_stdlib>
using namespace metal;

struct Params {
    float2 screenSize;
    float time;
    float previousTime;
    float2 jitter;
    float2 previousJitter;
    float historyWeight;
    float historyValid;
};
struct Input { float4 position [[position]]; float2 uv ; };
struct Output { float4 c0 [[color(0)]]; };

fragment Output fragmentMain(Input input [[stage_in]], constant Params& p [[buffer(15)]], texture2d<float> currentColor [[texture(0)]], texture2d<float> motionVectors [[texture(1)]], texture2d<float> previousColor [[texture(2)]], sampler imageSampler [[sampler(8)]]) {
float2 uv=input.uv; Output o;

    float4 current=currentColor.sample(imageSampler,uv,level(0)), motion=motionVectors.sample(imageSampler,uv,level(0));
float2 historyUV=uv+motion.xy+(p.jitter-p.previousJitter)/p.screenSize;
    float4 history=previousColor.sample(imageSampler,historyUV,level(0));
float3 lower=current.rgb,upper=current.rgb;
for(int y=-1;y<=1;y++) for(int x=-1;x<=1;x++) {
    float3 c=currentColor.sample(imageSampler,uv+float2(float(x),float(y))/p.screenSize,level(0)).rgb;
    lower=min(lower,c);upper=max(upper,c);
}
float weight=p.historyWeight;
    if(p.historyValid<0.5 || abs(history.a-current.a)>0.1 || any(historyUV<float2(0.0)) || any(historyUV>float2(1.0))) weight=0.0;
    history.rgb=clamp(history.rgb,lower,upper);
o.c0=float4(mix(current.rgb,history.rgb,weight),current.a);

return o;
}
