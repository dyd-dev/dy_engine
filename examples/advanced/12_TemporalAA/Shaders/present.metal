#include <metal_stdlib>
using namespace metal;

struct Input { float4 position [[position]]; float2 uv ; };
struct Output { float4 c0 [[color(0)]]; };

fragment Output fragmentMain(Input input [[stage_in]], texture2d<float> image0 [[texture(0)]], sampler imageSampler [[sampler(8)]]) {
float2 uv=input.uv; Output o;

    float3 color=image0.sample(imageSampler,uv,level(0)).rgb;
    for(int channel=0;channel<3;channel++) {
        float x=max(color[channel],0.0);
        color[channel]=x<=0.0031308 ? x*12.92 : 1.055*pow(x,1.0/2.4)-0.055;
    }
    o.c0=float4(color,1.0);

return o;
}
