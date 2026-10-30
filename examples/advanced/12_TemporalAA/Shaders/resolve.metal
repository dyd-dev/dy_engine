#include <metal_stdlib>
using namespace metal;

bool2 lessThan(float2 a,float2 b) { return a<b; }
bool3 lessThan(float3 a,float3 b) { return a<b; }
bool2 greaterThan(float2 a,float2 b) { return a>b; }

struct Params { float4 sizeTime; float4 settings; float4 jitter; float4 extra; };
struct Input { float4 position [[position]]; float2 uv ; };
struct Output { float4 c0 [[color(0)]]; };
#define SAMPLE0(U) image0.sample(imageSampler,U,level(0))
#define SAMPLE1(U) image1.sample(imageSampler,U,level(0))
#define SAMPLE2(U) image2.sample(imageSampler,U,level(0))

fragment Output fragmentMain(Input input [[stage_in]], constant Params& p [[buffer(15)]], texture2d<float> image0 [[texture(0)]], texture2d<float> image1 [[texture(1)]], texture2d<float> image2 [[texture(2)]], sampler imageSampler [[sampler(8)]]) {
float2 uv=input.uv; Output o;

float4 current=SAMPLE0(uv), motion=SAMPLE1(uv);
float2 historyUV=uv+motion.xy+(p.jitter.xy-p.jitter.zw)/p.sizeTime.xy;
float4 history=SAMPLE2(historyUV);
float3 lower=current.rgb,upper=current.rgb;
for(int y=-1;y<=1;y++) for(int x=-1;x<=1;x++) {
    float3 c=SAMPLE0(uv+float2(float(x),float(y))/p.sizeTime.xy).rgb;
    lower=min(lower,c);upper=max(upper,c);
}
float weight=p.settings.w;
if(p.extra.x<0.5 || abs(history.a-current.a)>0.1 || any(lessThan(historyUV,float2(0.0))) || any(greaterThan(historyUV,float2(1.0)))) weight=0.0;
if(p.sizeTime.w!=2.0) history.rgb=clamp(history.rgb,lower,upper);
if(p.sizeTime.w==1.0) weight=0.0;
o.c0=float4(mix(current.rgb,history.rgb,weight),current.a);

return o;
}
