#include <metal_stdlib>
using namespace metal;
struct EnvironmentVertex {float4 position [[position]];float2 uv;};
struct FrameConstants
{
    float4x4 inverseViewProjection;
    float4 cameraPosition;
    float4 sh[9];
    float4 settings;
    float4 screen;
    float4 probePosition;
    float4 probeBoxMin;
    float4 probeBoxMax;
    float4 specularColor;
    float4x4 backgroundRayToWorld;
};
inline float3 SampleEnvironmentRadiance(texture2d<float> image,float2 coordinate)
{
    const int2 size=int2(image.get_width(),image.get_height());
    const float2 pixel=coordinate*float2(size)-0.5f,weight=fract(pixel);
    int2 low=int2(floor(pixel)),high=low+1;
    low.x=(low.x%size.x+size.x)%size.x;high.x=(high.x%size.x+size.x)%size.x;
    low.y=clamp(low.y,0,size.y-1);high.y=clamp(high.y,0,size.y-1);
    return mix(mix(image.read(uint2(low)),image.read(uint2(high.x,low.y)),weight.x),
        mix(image.read(uint2(low.x,high.y)),image.read(uint2(high)),weight.x),weight.y).rgb;
}
fragment float4 environmentBackground(EnvironmentVertex input [[stage_in]],
    depth2d<float> depthTexture [[texture(0)]],texture2d<float> radianceTexture [[texture(6)]],
    sampler environmentSampler [[sampler(7)]],constant FrameConstants& frame [[buffer(9)]])
{
    if(depthTexture.read(uint2(input.position.xy))<1) discard_fragment();
    const float2 coordinate=input.position.xy*frame.screen.xy;
    const float2 clip=coordinate*float2(2,-2)+float2(-1,1);
    const float4 a=frame.backgroundRayToWorld*float4(clip,0,1),b=frame.backgroundRayToWorld*float4(clip,1,1);
    float3 direction=normalize(b.xyz*a.w-a.xyz*b.w);
    const float c=cos(frame.settings.z),s=sin(frame.settings.z);
    direction.xy=float2(c*direction.x+s*direction.y,-s*direction.x+c*direction.y);
    const float2 skyUV=float2(atan2(direction.y,direction.x)*0.15915494309189535f+0.5f,
        acos(clamp(direction.z,-1.0f,1.0f))*0.3183098861837907f);
    return float4(clamp(SampleEnvironmentRadiance(radianceTexture,skyUV)*frame.settings.y,0.0f,65504.0f),1);
}
