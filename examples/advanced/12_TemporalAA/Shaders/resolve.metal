#include <metal_stdlib>
using namespace metal;

struct Params { float4 sizeTime; float4 settings; float4 jitter; float4 extra; };
struct Input { float4 position [[position]]; float2 uv ; };
struct Output { float4 c0 [[color(0)]]; };

fragment Output fragmentMain(Input input [[stage_in]], constant Params& p [[buffer(15)]], texture2d<float> image0 [[texture(0)]], texture2d<float> image1 [[texture(1)]], texture2d<float> image2 [[texture(2)]], sampler imageSampler [[sampler(8)]]) {
float2 uv=input.uv; Output o;

    float4 current=image0.sample(imageSampler,uv,level(0)), motion=image1.sample(imageSampler,uv,level(0));
float2 historyUV=uv+motion.xy+(p.jitter.xy-p.jitter.zw)/p.sizeTime.xy;
    float4 history=image2.sample(imageSampler,historyUV,level(0));
float3 lower=current.rgb,upper=current.rgb;
for(int y=-1;y<=1;y++) for(int x=-1;x<=1;x++) {
    float3 c=image0.sample(imageSampler,uv+float2(float(x),float(y))/p.sizeTime.xy,level(0)).rgb;
    lower=min(lower,c);upper=max(upper,c);
}
float weight=p.settings.w;
    if(p.extra.x<0.5 || abs(history.a-current.a)>0.1 || any(historyUV<float2(0.0)) || any(historyUV>float2(1.0))) weight=0.0;
    history.rgb=clamp(history.rgb,lower,upper);
o.c0=float4(mix(current.rgb,history.rgb,weight),current.a);

return o;
}
