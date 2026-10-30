#include <metal_stdlib>
using namespace metal;

struct Params { float4 sizeTime; float4 settings; float4 jitter; float4 extra; };
struct Input { float4 position [[position]]; float2 uv ; };
struct Output { float4 c0 [[color(0)]]; };
#define SAMPLE0(U) image0.sample(imageSampler,U,level(0))
#define SAMPLE1(U) image1.sample(imageSampler,U,level(0))

float SrgbChannel(float x) { x=max(x,0.0); return x<=0.0031308 ? x*12.92 : 1.055*pow(x,1.0/2.4)-0.055; }
float3 srgb(float3 c) { return float3(SrgbChannel(c.r),SrgbChannel(c.g),SrgbChannel(c.b)); }

fragment Output fragmentMain(Input input [[stage_in]], constant Params& p [[buffer(15)]], texture2d<float> image0 [[texture(0)]], texture2d<float> image1 [[texture(1)]], sampler imageSampler [[sampler(8)]]) {
float2 uv=input.uv; Output o;

float3 color=SAMPLE0(uv).rgb;
if(p.sizeTime.w==3.0) color=float3(SAMPLE1(uv).xy*60.0+0.5,0.5);
o.c0=float4(srgb(color),1.0);

return o;
}
