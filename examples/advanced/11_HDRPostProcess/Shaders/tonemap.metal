#include <metal_stdlib>
using namespace metal;

struct Params {
    float2 screenSize;
    float exposure;
    float threshold;
    float bloomStrength;
    float emissiveIntensity;
};
struct Input { float4 position [[position]]; float2 uv ; };
struct Output { float4 c0 [[color(0)]]; };

fragment Output fragmentMain(Input input [[stage_in]], constant Params& p [[buffer(15)]], texture2d<float> image0 [[texture(0)]], texture2d<float> image1 [[texture(1)]], sampler imageSampler [[sampler(8)]]) {
float2 uv=input.uv; Output o;

    float3 hdr=image0.sample(imageSampler,uv,level(0)).rgb, bloom=image1.sample(imageSampler,uv,level(0)).rgb;
    hdr+=bloom*p.bloomStrength;
hdr*=p.exposure;
    float3 color=clamp((hdr*(2.51*hdr+0.03))/(hdr*(2.43*hdr+0.59)+0.14),float3(0.0),float3(1.0));
    for(int channel=0;channel<3;channel++) {
        float x=max(color[channel],0.0);
        color[channel]=x<=0.0031308 ? x*12.92 : 1.055*pow(x,1.0/2.4)-0.055;
    }
    o.c0=float4(color,1.0);

return o;
}
