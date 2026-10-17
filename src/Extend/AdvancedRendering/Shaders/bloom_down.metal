#include <metal_stdlib>
using namespace metal;
struct Input {float4 position [[position]];float2 uv [[user(locn0)]];};
struct Settings {float4 texelThreshold;float4 mode;};
constant float2 offsets[13]={
    float2(-2,-2),float2(0,-2),float2(2,-2),float2(-2,0),float2(0,0),float2(2,0),
    float2(-2,2),float2(0,2),float2(2,2),float2(-1,-1),float2(1,-1),float2(-1,1),float2(1,1)};
constant float weights[13]={.03125,.0625,.03125,.0625,.125,.0625,.03125,.0625,.03125,.125,.125,.125,.125};
fragment float4 fragmentMain(Input input [[stage_in]],texture2d<float> image [[texture(0)]],
    sampler imageSampler [[sampler(8)]],constant Settings& settings [[buffer(15)]])
{
    float3 filtered=0;
    float total=0;
    for(int i=0;i<13;++i)
    {
        float3 sampleColor=clamp(image.sample(imageSampler,input.uv+offsets[i]*settings.texelThreshold.xy,level(0)).rgb,0.f,64000.f);
        float weight=weights[i];
        if(settings.mode.x>.5) weight/=1+dot(sampleColor,float3(.2126,.7152,.0722));
        filtered+=sampleColor*weight;total+=weight;
    }
    filtered/=max(total,1e-6f);
    if(settings.mode.x>.5)
    {
        float brightness=max(max(filtered.r,filtered.g),filtered.b);
        float threshold=settings.texelThreshold.z,knee=settings.texelThreshold.w;
        float soft=clamp(brightness-threshold+knee,0.f,2*knee);
        soft=soft*soft/max(4*knee,1e-5f);
        filtered*=max(soft,brightness-threshold)/max(brightness,1e-5f);
    }
    return float4(filtered,1);
}
