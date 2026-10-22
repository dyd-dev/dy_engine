#include "probe_visibility.hlsl"
Texture2D<float> depthTexture : register(t0);
Texture2D<float4> normalTexture : register(t1);
Texture2D<float4> albedoTexture : register(t2);
Texture2D<float4> aoTexture : register(t3);
Texture2D<float4> prefilterTexture : register(t4);
Texture2D<float4> brdfTexture : register(t5);
SamplerState environmentSampler : register(s7);
SamplerState clampSampler : register(s8);
SamplerState aoSampler : register(s10);
SamplerState environmentLinearSampler:register(s15);
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
float3 DecodeNormal(float2 oct)
{
    oct=oct*2-1;
    float3 n=float3(oct,1-abs(oct.x)-abs(oct.y));
    if(n.z<0) n.xy=(1-abs(n.yx))*float2(n.x>=0 ? 1 : -1,n.y>=0 ? 1 : -1);
    return normalize(n);
}
float4 LinearFloatTexture(Texture2D<float4> image,float2 coordinate,uint mip,bool repeatU)
{
    uint width,height,levels;image.GetDimensions(mip,width,height,levels);
    int2 size=int2(width,height);
    float2 pixel=coordinate*size-0.5,weight=frac(pixel);
    int2 low=int2(floor(pixel)),high=low+1;
    if(repeatU)
    {
        int2 wrappedX=DyWrapBilinearX(low.x,high.x,size.x);
        low.x=wrappedX.x;high.x=wrappedX.y;
    }
    else {low.x=clamp(low.x,0,size.x-1);high.x=clamp(high.x,0,size.x-1);}
    low.y=clamp(low.y,0,size.y-1);high.y=clamp(high.y,0,size.y-1);
    return lerp(lerp(image.Load(int3(low,mip)),image.Load(int3(high.x,low.y,mip)),weight.x),
        lerp(image.Load(int3(low.x,high.y,mip)),image.Load(int3(high,mip)),weight.x),weight.y);
}
float2 EnvironmentUV(float3 direction)
{
    float c=cos(settings.z),s=sin(settings.z);
    direction.xy=float2(c*direction.x+s*direction.y,-s*direction.x+c*direction.y);
    return float2(atan2(direction.y,direction.x)*0.15915494309189535+0.5,acos(clamp(direction.z,-1.0,1.0))*0.3183098861837907);
}
float3 BoxDirection(float3 position,float3 direction,float3 center,float3 minimum,float3 maximum)
{
    if(!all(position>minimum) || !all(position<maximum)) return direction;
    float3 safeDirection=float3(direction.x>=0 ? 1 : -1,direction.y>=0 ? 1 : -1,direction.z>=0 ? 1 : -1)*max(abs(direction),1e-6);
    float3 wall=float3(direction.x>=0 ? maximum.x : minimum.x,
        direction.y>=0 ? maximum.y : minimum.y,direction.z>=0 ? maximum.z : minimum.z);
    float3 distances=(wall-position)/safeDirection;
    distances=float3(abs(direction.x)>1e-6 ? distances.x : 1e30,
        abs(direction.y)>1e-6 ? distances.y : 1e30,abs(direction.z)>1e-6 ? distances.z : 1e30);
    float distance=min(distances.x,min(distances.y,distances.z));
    float3 projected=position+direction*distance-center;
    if(distance<=0 || !isfinite(distance) || !all(isfinite(projected)) || dot(projected,projected)<1e-12) return direction;
    return normalize(projected);
}
float3 ProbeDirection(float3 position,float3 direction)
{
    return probePosition.w>.5?BoxDirection(position,direction,probePosition.xyz,probeBoxMin.xyz,probeBoxMax.xyz):direction;
}
float3 SpatialMip(float2 coordinate,int mip,int layer)
{
    uint width,height,layers,levels;probePrefilterTexture.GetDimensions(uint(mip),width,height,layers,levels);
    int2 dimensions=int2(width,height);float2 pixel=coordinate*float2(dimensions)-.5,w=frac(pixel);
    int2 a=int2(floor(pixel)),b=a+1;
    int2 wrappedX=DyWrapBilinearX(a.x,b.x,dimensions.x);
    a.x=wrappedX.x;b.x=wrappedX.y;
    a.y=clamp(a.y,0,dimensions.y-1);b.y=clamp(b.y,0,dimensions.y-1);
    return lerp(lerp(probePrefilterTexture.Load(int4(a,layer,mip)),probePrefilterTexture.Load(int4(b.x,a.y,layer,mip)),w.x),
        lerp(probePrefilterTexture.Load(int4(a.x,b.y,layer,mip)),probePrefilterTexture.Load(int4(b,layer,mip)),w.x),w.y).rgb;
}
bool NativeEnvironmentSample(float2 coordinate,float mip,float maximumMip,uint enabled)
{
    return enabled==1u && all(coordinate>=0) && all(coordinate<=1)
        && mip>=0 && mip<=maximumMip && maximumMip<=32;
}
float ProbeVisibility(float3 position,SpatialProbe probe,uint index)
{
    return SpatialProbeVisibility(position,probe.positionIntensity.xyz,probe.sh[0].w,probe.sh[1].w,
        probeParameters.x+index,probePrefilterTexture,clampSampler,environmentLinearSampler,probeParameters.w);
}
float3 BlendProbeRadiance(float3 position,float3 direction,float roughness)
{
    float mip=roughness*settings.w;
    uint lo=uint(floor(mip)),hi=min(lo+1,uint(settings.w));
    float2 envUV=EnvironmentUV(ProbeDirection(position,direction));
    float3 sum=0;float weights=0,coverage=0;
    for(uint i=0;i<probeParameters.x;++i)
    {
        SpatialProbe probe=probes[i];float3 margin=min(position-probe.boxMinFade.xyz,probe.boxMaxMip.xyz-position);
        float edge=saturate(min(margin.x,min(margin.y,margin.z))/probe.boxMinFade.w);if(edge<=0)continue;
        float3 delta=position-probe.positionIntensity.xyz,extent=probe.boxMaxMip.xyz-probe.boxMinFade.xyz;
        float weight=edge/max(dot(delta,delta),max(dot(extent,extent)*.0001,1e-6));
        float3 d=BoxDirection(position,direction,probe.positionIntensity.xyz,probe.boxMinFade.xyz,probe.boxMaxMip.xyz);
        float2 coordinate=float2(atan2(d.y,d.x)*.15915494309189535+.5,acos(clamp(d.z,-1,1))*.3183098861837907);
        float mip=roughness*probe.boxMaxMip.w;int lo=int(floor(mip)),hi=min(lo+1,int(probe.boxMaxMip.w));
        float3 sampledRadiance;
        if(NativeEnvironmentSample(coordinate,mip,probe.boxMaxMip.w,probeParameters.z))
            sampledRadiance=probePrefilterTexture.SampleLevel(environmentLinearSampler,float3(coordinate,float(i)),mip).rgb;
        else sampledRadiance=lerp(SpatialMip(coordinate,lo,int(i)),SpatialMip(coordinate,hi,int(i)),frac(mip));
        sum+=sampledRadiance*
            probe.positionIntensity.w*(weight*ProbeVisibility(position,probe,i));
        weights+=weight;coverage+=edge;
    }
    if(probeParameters.y!=0u && weights>0 && coverage>=1 && all(isfinite(envUV)) && isfinite(mip))
    {
        float3 local=sum/weights;
        if(all(abs(local)<=2.1267646e37))return local;
    }
    float3 fallback;
    if(NativeEnvironmentSample(envUV,mip,settings.w,probeParameters.z))
        fallback=prefilterTexture.SampleLevel(environmentLinearSampler,envUV,mip).rgb;
    else fallback=lerp(LinearFloatTexture(prefilterTexture,envUV,lo,true),
        LinearFloatTexture(prefilterTexture,envUV,hi,true),frac(mip)).rgb;
    fallback*=settings.y;
    return weights>0?lerp(fallback,sum/weights,saturate(coverage)):fallback;
}
float4 main(float4 fragment:SV_Position,float2 uv:TEXCOORD0):SV_Target
{
    int2 pixel=int2(fragment.xy);
    float depth=depthTexture.Load(int3(pixel,0));
    if(depth>=1 || settings.x<0.5) return 0;
    float4 surface=normalTexture.Load(int3(pixel,0)),albedo=albedoTexture.Load(int3(pixel,0));
    if(surface.z<0)return 0;
    float4 world=mul(inverseViewProjection,float4(uv*float2(2,-2)+float2(-1,1),depth,1));
    float3 position=world.xyz/world.w;
    float3 normal=DecodeNormal(surface.xy),view=normalize(cameraPosition.xyz-position);
    float roughness=clamp(surface.z,0.01,1.0),nDotV=max(dot(normal,view),0.0);
    float3 reflection=reflect(-view,normal);
    float3 radiance=BlendProbeRadiance(position,reflection,roughness);
    float2 brdf=LinearFloatTexture(brdfTexture,float2(nDotV,roughness),0,false).rg;
    float3 f0=lerp(0.04,albedo.rgb,saturate(albedo.a));
    float ao=saturate(surface.w);
    // RGBA16F AO uses normalized UV and native bilinear filtering at its own resolution.
    if(screen.z>0.5) ao*=saturate(aoTexture.SampleLevel(aoSampler,uv,0).r);
    return float4(clamp(radiance*(f0*brdf.x+brdf.y)*ao*specularColor.rgb,0,65504),1);
}
