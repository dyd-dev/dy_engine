#include "probe_visibility.hlsl"
Texture2D<float> depthTexture : register(t0);
Texture2D<float4> normalTexture : register(t1);
Texture2D<float4> aoTexture : register(t3);
SamplerState clampSampler : register(s8);
SamplerState aoSampler : register(s10);
cbuffer FrameConstants : register(b9)
{
    column_major float4x4 inverseViewProjection;
    float4 cameraPosition;
    float4 sh[9];
    float4 settings;
    float4 screen;
    float4 probePosition;
    float4 probeBoxMin;
    float4 probeBoxMax;
    float4 specularColor;
};
struct SpatialProbe {float4 positionIntensity;float4 boxMinFade;float4 boxMaxMip;float4 sh[9];};
StructuredBuffer<SpatialProbe> probes:register(t22);
Texture2DArray<float4> probePrefilterTexture:register(t23);
cbuffer ProbeParameters:register(b24) {uint4 probeParameters;};
float ProbeVisibility(float3 position,SpatialProbe probe,uint index)
{
    return SpatialProbeVisibility(position,probe.positionIntensity.xyz,probe.sh[0].w,probe.sh[1].w,
        probeParameters.x+index,probePrefilterTexture);
}
float ProbeWeight(float3 position,SpatialProbe probe,out float coverage)
{
    float3 margin=min(position-probe.boxMinFade.xyz,probe.boxMaxMip.xyz-position);
    coverage=saturate(min(margin.x,min(margin.y,margin.z))/probe.boxMinFade.w);
    float3 delta=position-probe.positionIntensity.xyz,extent=probe.boxMaxMip.xyz-probe.boxMinFade.xyz;
    return coverage/max(dot(delta,delta),max(dot(extent,extent)*.0001,1e-6));
}
float3 ProbeIrradiance(SpatialProbe probe,float3 n)
{
    return (probe.sh[0].rgb*.28209479177387814+probe.sh[1].rgb*(.4886025119029199*n.y)+
        probe.sh[2].rgb*(.4886025119029199*n.z)+probe.sh[3].rgb*(.4886025119029199*n.x)+
        probe.sh[4].rgb*(1.0925484305920792*n.x*n.y)+probe.sh[5].rgb*(1.0925484305920792*n.y*n.z)+
        probe.sh[6].rgb*(.31539156525252005*(3*n.z*n.z-1))+probe.sh[7].rgb*(1.0925484305920792*n.x*n.z)+
        probe.sh[8].rgb*(.5462742152960396*(n.x*n.x-n.y*n.y)))*probe.positionIntensity.w;
}
float3 DecodeNormal(float2 oct)
{
    oct=oct*2-1;
    float3 n=float3(oct,1-abs(oct.x)-abs(oct.y));
    if(n.z<0) n.xy=(1-abs(n.yx))*float2(n.x>=0 ? 1 : -1,n.y>=0 ? 1 : -1);
    return normalize(n);
}
float4 main(float4 fragment:SV_Position,float2 uv:TEXCOORD0):SV_Target
{
    int2 pixel=int2(fragment.xy);
    float depth=depthTexture.Load(int3(pixel,0));if(depth>=1) return 0;
    float4 surface=normalTexture.Load(int3(pixel,0));if(surface.z<0)return 0;
    float3 n=DecodeNormal(surface.xy);
    float3 worldNormal=n;
    float c=cos(settings.z),s=sin(settings.z);n.xy=float2(c*n.x+s*n.y,-s*n.x+c*n.y);
    float3 irradiance=sh[0].rgb*0.28209479177387814+
        sh[1].rgb*(0.4886025119029199*n.y)+sh[2].rgb*(0.4886025119029199*n.z)+
        sh[3].rgb*(0.4886025119029199*n.x)+sh[4].rgb*(1.0925484305920792*n.x*n.y)+
        sh[5].rgb*(1.0925484305920792*n.y*n.z)+sh[6].rgb*(0.31539156525252005*(3*n.z*n.z-1))+
        sh[7].rgb*(1.0925484305920792*n.x*n.z)+sh[8].rgb*(0.5462742152960396*(n.x*n.x-n.y*n.y));
    irradiance*=settings.y;
    if(probeParameters.x>0)
    {
        float4 world=mul(inverseViewProjection,float4(uv*float2(2,-2)+float2(-1,1),depth,1));
        float3 position=world.xyz/world.w,sum=0;float weights=0,coverage=0;
        for(uint i=0;i<probeParameters.x;++i)
        {
            float edge;float weight=ProbeWeight(position,probes[i],edge);if(weight<=0)continue;
            sum+=ProbeIrradiance(probes[i],worldNormal)*(weight*ProbeVisibility(position,probes[i],i));
            weights+=weight;coverage+=edge;
        }
        if(weights>0)irradiance=lerp(irradiance,sum/weights,saturate(coverage));
    }
    float ao=saturate(surface.w);if(screen.z>0.5) ao*=saturate(aoTexture.SampleLevel(aoSampler,uv,0).r);
    return float4(clamp(max(irradiance,0)*(0.3183098861837907*ao),0,65504),1);
}
