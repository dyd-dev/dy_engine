#include <metal_stdlib>
using namespace metal;
#include "probe_visibility.metal"
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
};
struct SpatialProbe {float4 positionIntensity,boxMinFade,boxMaxMip;float4 sh[9];};
inline float ProbeVisibility(float3 position,device const SpatialProbe& probe,uint index,
    texture2d_array<float> image,uint count)
{
    return SpatialProbeVisibility(position,probe.positionIntensity.xyz,probe.sh[0].w,probe.sh[1].w,
        count+index,image);
}
inline float ProbeWeight(float3 position,device const SpatialProbe& probe,thread float& coverage)
{
    const float3 margin=min(position-probe.boxMinFade.xyz,probe.boxMaxMip.xyz-position);
    coverage=clamp(min(margin.x,min(margin.y,margin.z))/probe.boxMinFade.w,0.f,1.f);
    const float3 delta=position-probe.positionIntensity.xyz,extent=probe.boxMaxMip.xyz-probe.boxMinFade.xyz;
    return coverage/max(dot(delta,delta),max(dot(extent,extent)*.0001f,1e-6f));
}
inline float3 ProbeIrradiance(device const SpatialProbe& probe,float3 n)
{
    return (probe.sh[0].rgb*.28209479177387814f+probe.sh[1].rgb*(.4886025119029199f*n.y)+
        probe.sh[2].rgb*(.4886025119029199f*n.z)+probe.sh[3].rgb*(.4886025119029199f*n.x)+
        probe.sh[4].rgb*(1.0925484305920792f*n.x*n.y)+probe.sh[5].rgb*(1.0925484305920792f*n.y*n.z)+
        probe.sh[6].rgb*(.31539156525252005f*(3*n.z*n.z-1))+probe.sh[7].rgb*(1.0925484305920792f*n.x*n.z)+
        probe.sh[8].rgb*(.5462742152960396f*(n.x*n.x-n.y*n.y)))*probe.positionIntensity.w;
}
inline float3 DecodeDiffuseEnvironmentNormal(float2 oct)
{
    oct=oct*2-1;
    float3 n=float3(oct,1-abs(oct.x)-abs(oct.y));
    if(n.z<0) n.xy=(1-abs(n.yx))*float2(n.x>=0 ? 1 : -1,n.y>=0 ? 1 : -1);
    return normalize(n);
}
fragment float4 environmentDiffuse(EnvironmentVertex input [[stage_in]],
    depth2d<float> depthTexture [[texture(0)]],texture2d<float> normalTexture [[texture(1)]],
    texture2d<float> aoTexture [[texture(3)]],sampler clampSampler [[sampler(8)]],
    sampler aoSampler [[sampler(10)]],
    constant FrameConstants& frame [[buffer(9)]],device const SpatialProbe* probes [[buffer(22)]],
    texture2d_array<float> probePrefilterTexture [[texture(23)]],
    constant uint4& probeParameters [[buffer(24)]])
{
    const uint2 pixel=uint2(input.position.xy);
    const float depth=depthTexture.read(pixel);
    if(depth>=1) return 0;
    const float4 surface=normalTexture.read(pixel);if(surface.z<0)return 0;
    float3 n=DecodeDiffuseEnvironmentNormal(surface.xy);
    const float3 worldNormal=n;
    const float c=cos(frame.settings.z),s=sin(frame.settings.z);n.xy=float2(c*n.x+s*n.y,-s*n.x+c*n.y);
    float3 irradiance=frame.sh[0].rgb*0.28209479177387814f+
        frame.sh[1].rgb*(0.4886025119029199f*n.y)+frame.sh[2].rgb*(0.4886025119029199f*n.z)+
        frame.sh[3].rgb*(0.4886025119029199f*n.x)+frame.sh[4].rgb*(1.0925484305920792f*n.x*n.y)+
        frame.sh[5].rgb*(1.0925484305920792f*n.y*n.z)+frame.sh[6].rgb*(0.31539156525252005f*(3*n.z*n.z-1))+
        frame.sh[7].rgb*(1.0925484305920792f*n.x*n.z)+frame.sh[8].rgb*(0.5462742152960396f*(n.x*n.x-n.y*n.y));
    irradiance*=frame.settings.y;
    if(probeParameters.x>0)
    {
        const float4 world=frame.inverseViewProjection*float4(input.uv*float2(2,-2)+float2(-1,1),depth,1);
        const float3 position=world.xyz/world.w;
        float3 sum=0;float weights=0,coverage=0;
        for(uint i=0;i<probeParameters.x;++i)
        {
            float edge;const float weight=ProbeWeight(position,probes[i],edge);
            if(weight<=0) continue;
            sum+=ProbeIrradiance(probes[i],worldNormal)*
                (weight*ProbeVisibility(position,probes[i],i,probePrefilterTexture,probeParameters.x));
            weights+=weight;coverage+=edge;
        }
        if(weights>0) irradiance=mix(irradiance,sum/weights,clamp(coverage,0.f,1.f));
    }
    float ao=saturate(surface.w);if(frame.screen.z>0.5) ao*=saturate(aoTexture.sample(aoSampler,input.uv,level(0)).r);
    return float4(clamp(max(irradiance,0.0f)*(0.3183098861837907f*ao),0.0f,65504.0f),1);
}
