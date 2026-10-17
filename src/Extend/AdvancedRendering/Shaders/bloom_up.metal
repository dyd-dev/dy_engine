#include <metal_stdlib>
using namespace metal;
struct Input {float4 position [[position]];float2 uv [[user(locn0)]];};
struct Settings {float4 texel;float4 unused;};
fragment float4 fragmentMain(Input input [[stage_in]],texture2d<float> baseImage [[texture(0)]],
    texture2d<float> lowImage [[texture(1)]],sampler imageSampler [[sampler(8)]],
    constant Settings& settings [[buffer(15)]])
{
    float3 blurred=0;
    for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x)
    {
        float weight=float(x==0?2:1)*float(y==0?2:1);
        blurred+=lowImage.sample(imageSampler,input.uv+float2(x,y)*settings.texel.xy,level(0)).rgb*weight;
    }
    float3 base=baseImage.sample(imageSampler,input.uv,level(0)).rgb;
    return float4(mix(base,blurred/16.f,.65f),1);
}
