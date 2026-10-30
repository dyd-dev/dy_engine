#include <metal_stdlib>
using namespace metal;

struct Params { float4 sizeTime; float4 settings; float4 jitter; float4 extra; };
struct Input { float4 position [[position]]; float2 uv ; };
struct Output { float4 c0 [[color(0)]]; };
#define SAMPLE0(U) image0.sample(imageSampler,U,level(0))

fragment Output fragmentMain(Input input [[stage_in]], constant Params& p [[buffer(15)]], texture2d<float> image0 [[texture(0)]], sampler imageSampler [[sampler(8)]]) {
float2 uv=input.uv; Output o;

float3 sum=float3(0.0);float weightSum=0.0;
for(int i=-6;i<=6;i++) {
    float w=exp(-float(i*i)/12.0);
    sum+=SAMPLE0(uv+float2(0.0,float(i)/p.sizeTime.y)).rgb*w;weightSum+=w;
}
o.c0=float4(sum/weightSum,1.0);

return o;
}
