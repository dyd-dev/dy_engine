#include <metal_stdlib>
using namespace metal;
struct Input {float4 position [[position]];float2 uv [[user(locn0)]];};
struct Settings {float4 operation;};
constant float minimumLogLuminance=-13.28771238f; // log2(0.0001)
fragment float4 fragmentMain(Input input [[stage_in]],
    texture2d<float> sourceImage [[texture(0)]],texture2d<float> previousImage [[texture(1)]],
    sampler imageSampler [[sampler(8)]],constant Settings& settings [[buffer(15)]])
{
    if(settings.operation.x>1.5f)
    {
        float target=sourceImage.read(uint2(0)).r;
        if(!isfinite(target)) target=minimumLogLuminance;
        float adapted=target;
        if(settings.operation.y>.5f)
        {
            float previous=previousImage.read(uint2(0)).r;
            if(isfinite(previous)) adapted=mix(previous,target,settings.operation.z);
        }
        return float4(adapted,target,0,1);
    }
    uint2 size=uint2(sourceImage.get_width(),sourceImage.get_height());
    uint2 origin=uint2(input.position.xy)*4;
    float weightedLog=0,weight=0;
    for(uint y=0;y<4;++y) for(uint x=0;x<4;++x)
    {
        uint2 pixel=origin+uint2(x,y);
        if(any(pixel>=size)) continue;
        float4 value=sourceImage.read(pixel);
        if(settings.operation.x<.5f)
        {
            if(!all(isfinite(value.rgb))) continue;
            float luminance=dot(clamp(value.rgb,0.f,65504.f),float3(.2126,.7152,.0722));
            weightedLog+=log2(max(luminance,.0001f));weight+=1;
        }
        else
        {
            weightedLog+=value.r*value.g;weight+=value.g;
        }
    }
    return float4(weight>0?weightedLog/weight:minimumLogLuminance,weight/16,0,1);
}
