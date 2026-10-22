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
float3 DecodeEnvironmentNormal(float2 oct)
{
    oct=oct*2-1;
    float3 n=float3(oct,1-abs(oct.x)-abs(oct.y));
    if(n.z<0) n.xy=(1-abs(n.yx))*float2(n.x>=0 ? 1 : -1,n.y>=0 ? 1 : -1);
    return normalize(n);
}
inline float4 LinearFloatTexture(texture2d<float> image,float2 coordinate,uint mip,bool repeatU)
{
    const int2 size=int2(image.get_width(mip),image.get_height(mip));
    const float2 pixel=coordinate*float2(size)-0.5f,weight=fract(pixel);
    int2 low=int2(floor(pixel)),high=low+1;
    if(repeatU)
    {
        const int2 wrappedX=DyWrapBilinearX(low.x,high.x,size.x);
        low.x=wrappedX.x;high.x=wrappedX.y;
    }
    else {low.x=clamp(low.x,0,size.x-1);high.x=clamp(high.x,0,size.x-1);}
    low.y=clamp(low.y,0,size.y-1);high.y=clamp(high.y,0,size.y-1);
    return mix(mix(image.read(uint2(low),mip),image.read(uint2(high.x,low.y),mip),weight.x),
        mix(image.read(uint2(low.x,high.y),mip),image.read(uint2(high),mip),weight.x),weight.y);
}
inline float3 BoxDirection(float3 position,float3 direction,float3 center,float3 minimum,float3 maximum)
{
    if(!all(position>minimum) || !all(position<maximum)) return direction;
    const float3 safeDirection=select(float3(-1),float3(1),direction>=float3(0))*max(abs(direction),float3(1e-6f));
    const float3 wall=select(minimum,maximum,direction>=float3(0));
    const float3 distances=select(float3(1e30f),(wall-position)/safeDirection,abs(direction)>float3(1e-6f));
    const float distance=min(distances.x,min(distances.y,distances.z));
    const float3 projected=position+direction*distance-center;
    if(distance<=0 || !isfinite(distance) || !all(isfinite(projected)) || dot(projected,projected)<1e-12f) return direction;
    return normalize(projected);
}
inline float3 ProbeDirection(float3 position,float3 direction,constant FrameConstants& frame)
{
    return frame.probePosition.w>.5f?BoxDirection(position,direction,frame.probePosition.xyz,
        frame.probeBoxMin.xyz,frame.probeBoxMax.xyz):direction;
}
inline float3 SpatialMip(texture2d_array<float> image,float2 coordinate,uint mip,uint layer)
{
    const int2 dimensions=int2(image.get_width(mip),image.get_height(mip));
    const float2 pixel=coordinate*float2(dimensions)-.5f,w=fract(pixel);
    int2 a=int2(floor(pixel)),b=a+1;
    const int2 wrappedX=DyWrapBilinearX(a.x,b.x,dimensions.x);
    a.x=wrappedX.x;b.x=wrappedX.y;
    a.y=clamp(a.y,0,dimensions.y-1);b.y=clamp(b.y,0,dimensions.y-1);
    return mix(mix(image.read(uint2(a),layer,mip),image.read(uint2(b.x,a.y),layer,mip),w.x),
        mix(image.read(uint2(a.x,b.y),layer,mip),image.read(uint2(b),layer,mip),w.x),w.y).rgb;
}
inline bool NativeEnvironmentSample(float2 coordinate,float mip,float maximumMip,uint enabled)
{
    return enabled==1u && all(coordinate>=0) && all(coordinate<=1)
        && mip>=0 && mip<=maximumMip && maximumMip<=32;
}
inline float ProbeVisibility(float3 position,device const SpatialProbe& probe,uint index,
    texture2d_array<float> image,uint count,sampler pointSampler,sampler linearSampler,uint enabled)
{
    return SpatialProbeVisibility(position,probe.positionIntensity.xyz,probe.sh[0].w,probe.sh[1].w,
        count+index,image,pointSampler,linearSampler,enabled);
}
inline float3 BlendProbeRadiance(float3 position,float3 direction,float roughness,
    constant FrameConstants& frame,texture2d<float> prefilterTexture,
    device const SpatialProbe* probes,texture2d_array<float> image,constant uint4& parameters,
    sampler pointSampler,sampler environmentLinearSampler)
{
    const float3 projected=ProbeDirection(position,direction,frame);
    const float c=cos(frame.settings.z),s=sin(frame.settings.z);
    const float3 reflection=float3(c*projected.x+s*projected.y,-s*projected.x+c*projected.y,projected.z);
    const float2 envUV=float2(atan2(reflection.y,reflection.x)*0.15915494309189535f+0.5f,
        acos(clamp(reflection.z,-1.0f,1.0f))*0.3183098861837907f);
    const float mip=roughness*frame.settings.w;
    const uint lo=uint(floor(mip)),hi=min(lo+1,uint(frame.settings.w));
    const uint count=parameters.x;
    float3 sum=0;float weights=0,coverage=0;
    for(uint i=0;i<count;++i)
    {
        device const SpatialProbe& probe=probes[i];
        const float3 margin=min(position-probe.boxMinFade.xyz,probe.boxMaxMip.xyz-position);
        const float edge=clamp(min(margin.x,min(margin.y,margin.z))/probe.boxMinFade.w,0.f,1.f);
        if(edge<=0) continue;
        const float3 delta=position-probe.positionIntensity.xyz,extent=probe.boxMaxMip.xyz-probe.boxMinFade.xyz;
        const float weight=edge/max(dot(delta,delta),max(dot(extent,extent)*.0001f,1e-6f));
        const float3 d=BoxDirection(position,direction,probe.positionIntensity.xyz,probe.boxMinFade.xyz,probe.boxMaxMip.xyz);
        const float2 coordinate=float2(atan2(d.y,d.x)*.15915494309189535f+.5f,acos(clamp(d.z,-1.f,1.f))*.3183098861837907f);
        const float mip=roughness*probe.boxMaxMip.w;const uint lo=uint(floor(mip)),hi=min(lo+1,uint(probe.boxMaxMip.w));
        float3 sampledRadiance;
        if(NativeEnvironmentSample(coordinate,mip,probe.boxMaxMip.w,parameters.z))
            sampledRadiance=image.sample(environmentLinearSampler,coordinate,i,level(mip)).rgb;
        else sampledRadiance=mix(SpatialMip(image,coordinate,lo,i),SpatialMip(image,coordinate,hi,i),fract(mip));
        sum+=sampledRadiance*
            probe.positionIntensity.w*(weight*ProbeVisibility(position,probe,i,image,count,pointSampler,environmentLinearSampler,parameters.w));
        weights+=weight;coverage+=edge;
    }
    if(parameters.y!=0u && weights>0 && coverage>=1 && all(isfinite(envUV)) && isfinite(mip))
    {
        float3 local=sum/weights;
        if(all(abs(local)<=2.1267646e37f))return local;
    }
    float3 fallback;
    if(NativeEnvironmentSample(envUV,mip,frame.settings.w,parameters.z))
        fallback=prefilterTexture.sample(environmentLinearSampler,envUV,level(mip)).rgb;
    else fallback=mix(LinearFloatTexture(prefilterTexture,envUV,lo,true),
        LinearFloatTexture(prefilterTexture,envUV,hi,true),fract(mip)).rgb;
    fallback*=frame.settings.y;
    return weights>0?mix(fallback,sum/weights,clamp(coverage,0.f,1.f)):fallback;
}
fragment float4 environmentSpecular(EnvironmentVertex input [[stage_in]],
    depth2d<float> depthTexture [[texture(0)]],texture2d<float> normalTexture [[texture(1)]],
    texture2d<float> albedoTexture [[texture(2)]],texture2d<float> aoTexture [[texture(3)]],
    texture2d<float> prefilterTexture [[texture(4)]],texture2d<float> brdfTexture [[texture(5)]],
    sampler environmentSampler [[sampler(7)]],sampler clampSampler [[sampler(8)]],
    sampler aoSampler [[sampler(10)]],sampler environmentLinearSampler [[sampler(15)]],
    constant FrameConstants& frame [[buffer(9)]],device const SpatialProbe* probes [[buffer(22)]],
    texture2d_array<float> probePrefilterTexture [[texture(23)]],constant uint4& probeParameters [[buffer(24)]])
{
    const uint2 pixel=uint2(input.position.xy);
    const float depth=depthTexture.read(pixel);
    if(depth>=1 || frame.settings.x<0.5) return 0;
    const float4 surface=normalTexture.read(pixel),albedo=albedoTexture.read(pixel);
    if(surface.z<0)return 0;
    const float4 world=frame.inverseViewProjection*float4(input.uv*float2(2,-2)+float2(-1,1),depth,1);
    const float3 position=world.xyz/world.w;
    const float3 normal=DecodeEnvironmentNormal(surface.xy),view=normalize(frame.cameraPosition.xyz-position);
    const float roughness=clamp(surface.z,0.01f,1.0f),nDotV=max(dot(normal,view),0.0f);
    const float3 worldReflection=reflect(-view,normal);
    float3 radiance=BlendProbeRadiance(position,worldReflection,roughness,frame,prefilterTexture,probes,probePrefilterTexture,probeParameters,clampSampler,environmentLinearSampler);
    const float2 brdf=LinearFloatTexture(brdfTexture,float2(nDotV,roughness),0,false).rg;
    const float3 f0=mix(float3(0.04f),albedo.rgb,saturate(albedo.a));
    float ao=saturate(surface.w);
    // 저해상도 AO는 정규화된 UV와 자체 해상도의 선형 필터를 사용한다.
    if(frame.screen.z>0.5) ao*=saturate(aoTexture.sample(aoSampler,input.uv,level(0)).r);
    return float4(clamp(radiance*(f0*brdf.x+brdf.y)*ao*frame.specularColor.rgb,0.0f,65504.0f),1);
}
