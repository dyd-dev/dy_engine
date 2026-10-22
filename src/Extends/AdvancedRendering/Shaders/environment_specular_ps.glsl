#version 450
#extension GL_GOOGLE_include_directive : require
#include "probe_visibility.glsl"
layout(location=0) in vec2 uv;
layout(location=0) out vec4 outputColor;
layout(set=0,binding=0) uniform texture2D depthTexture;
layout(set=0,binding=1) uniform texture2D normalTexture;
layout(set=0,binding=2) uniform texture2D albedoTexture;
layout(set=0,binding=3) uniform texture2D aoTexture;
layout(set=0,binding=4) uniform texture2D prefilterTexture;
layout(set=0,binding=5) uniform texture2D brdfTexture;
layout(set=0,binding=7) uniform sampler environmentSampler;
layout(set=0,binding=8) uniform sampler clampSampler;
layout(set=0,binding=10) uniform sampler aoSampler;
layout(set=0,binding=15) uniform sampler environmentLinearSampler;
layout(set=0,binding=9) uniform FrameConstants
{
    mat4 inverseViewProjection;
    vec4 cameraPosition;
    vec4 sh[9];
    vec4 settings;
    vec4 screen;
    vec4 probePosition;
    vec4 probeBoxMin;
    vec4 probeBoxMax;
    vec4 specularColor;
} frame;
struct SpatialProbe {vec4 positionIntensity;vec4 boxMinFade;vec4 boxMaxMip;vec4 sh[9];};
layout(std430,set=0,binding=22) readonly buffer ProbeData {SpatialProbe probes[];};
layout(set=0,binding=23) uniform texture2DArray probePrefilterTexture;
layout(set=0,binding=24) uniform ProbeParameters {uvec4 probeParameters;};
vec3 DecodeNormal(vec2 oct)
{
    oct=oct*2.0-1.0;
    vec3 n=vec3(oct,1.0-abs(oct.x)-abs(oct.y));
    if(n.z<0.0) n.xy=(1.0-abs(n.yx))*mix(vec2(-1),vec2(1),greaterThanEqual(n.xy,vec2(0)));
    return normalize(n);
}
vec4 LinearFloatTexture(texture2D image,vec2 coordinate,int mip,bool repeatU)
{
    ivec2 size=textureSize(sampler2D(image,clampSampler),mip);
    vec2 pixel=coordinate*vec2(size)-0.5,weight=fract(pixel);
    ivec2 low=ivec2(floor(pixel)),high=low+1;
    if(repeatU)
    {
        ivec2 wrappedX=DyWrapBilinearX(low.x,high.x,size.x);
        low.x=wrappedX.x;high.x=wrappedX.y;
    }
    else {low.x=clamp(low.x,0,size.x-1);high.x=clamp(high.x,0,size.x-1);}
    low.y=clamp(low.y,0,size.y-1);high.y=clamp(high.y,0,size.y-1);
    return mix(mix(texelFetch(sampler2D(image,clampSampler),low,mip),texelFetch(sampler2D(image,clampSampler),ivec2(high.x,low.y),mip),weight.x),
        mix(texelFetch(sampler2D(image,clampSampler),ivec2(low.x,high.y),mip),texelFetch(sampler2D(image,clampSampler),high,mip),weight.x),weight.y);
}
vec2 EnvironmentUV(vec3 direction)
{
    float c=cos(frame.settings.z),s=sin(frame.settings.z);
    direction.xy=mat2(c,-s,s,c)*direction.xy;
    return vec2(atan(direction.y,direction.x)*0.15915494309189535+0.5,acos(clamp(direction.z,-1.0,1.0))*0.3183098861837907);
}
vec3 BoxDirection(vec3 position,vec3 direction,vec3 center,vec3 minimum,vec3 maximum)
{
    // 상자 교차는 파노라마 방향 변환 전의 월드 좌표에서 계산한다.
    if(!all(greaterThan(position,minimum)) || !all(lessThan(position,maximum))) return direction;
    vec3 safeDirection=mix(vec3(-1),vec3(1),greaterThanEqual(direction,vec3(0)))*max(abs(direction),vec3(1e-6));
    vec3 wall=mix(minimum,maximum,greaterThanEqual(direction,vec3(0)));
    vec3 distances=mix(vec3(1e30),(wall-position)/safeDirection,greaterThan(abs(direction),vec3(1e-6)));
    float distance=min(distances.x,min(distances.y,distances.z));
    vec3 projected=position+direction*distance-center;
    if(distance<=0.0 || isnan(distance) || isinf(distance) ||
        any(isnan(projected)) || any(isinf(projected)) || dot(projected,projected)<1e-12) return direction;
    return normalize(projected);
}
vec3 ProbeDirection(vec3 position,vec3 direction)
{
    return frame.probePosition.w>.5?BoxDirection(position,direction,frame.probePosition.xyz,
        frame.probeBoxMin.xyz,frame.probeBoxMax.xyz):direction;
}
vec3 SpatialMip(vec2 coordinate,int mip,int layer)
{
    ivec2 dimensions=textureSize(sampler2DArray(probePrefilterTexture,clampSampler),mip).xy;
    vec2 pixel=coordinate*vec2(dimensions)-.5,w=fract(pixel);ivec2 a=ivec2(floor(pixel)),b=a+1;
    ivec2 wrappedX=DyWrapBilinearX(a.x,b.x,dimensions.x);
    a.x=wrappedX.x;b.x=wrappedX.y;
    a.y=clamp(a.y,0,dimensions.y-1);b.y=clamp(b.y,0,dimensions.y-1);
    return mix(mix(texelFetch(sampler2DArray(probePrefilterTexture,clampSampler),ivec3(a,layer),mip),
        texelFetch(sampler2DArray(probePrefilterTexture,clampSampler),ivec3(b.x,a.y,layer),mip),w.x),
        mix(texelFetch(sampler2DArray(probePrefilterTexture,clampSampler),ivec3(a.x,b.y,layer),mip),
        texelFetch(sampler2DArray(probePrefilterTexture,clampSampler),ivec3(b,layer),mip),w.x),w.y).rgb;
}
bool NativeEnvironmentSample(vec2 coordinate,float mip,float maximumMip,uint enabled)
{
    return enabled==1u && all(greaterThanEqual(coordinate,vec2(0))) && all(lessThanEqual(coordinate,vec2(1)))
        && mip>=0 && mip<=maximumMip && maximumMip<=32;
}
float ProbeVisibility(vec3 position,SpatialProbe probe,uint index)
{
    return SpatialProbeVisibility(position,probe.positionIntensity.xyz,probe.sh[0].w,probe.sh[1].w,
        probeParameters.x+index,probePrefilterTexture,clampSampler);
}
vec3 BlendProbeRadiance(vec3 position,vec3 direction,float roughness)
{
    float mip=roughness*frame.settings.w;
    int lo=int(floor(mip)),hi=min(lo+1,int(frame.settings.w));
    vec2 envUV=EnvironmentUV(ProbeDirection(position,direction));
    vec3 sum=vec3(0);float weights=0,coverage=0;
    for(uint i=0u;i<probeParameters.x;++i)
    {
        SpatialProbe probe=probes[i];vec3 margin=min(position-probe.boxMinFade.xyz,probe.boxMaxMip.xyz-position);
        float edge=clamp(min(margin.x,min(margin.y,margin.z))/probe.boxMinFade.w,0,1);
        if(edge<=0)continue;
        vec3 delta=position-probe.positionIntensity.xyz,extent=probe.boxMaxMip.xyz-probe.boxMinFade.xyz;
        float weight=edge/max(dot(delta,delta),max(dot(extent,extent)*.0001,1e-6));
        vec3 d=BoxDirection(position,direction,probe.positionIntensity.xyz,probe.boxMinFade.xyz,probe.boxMaxMip.xyz);
        vec2 coordinate=vec2(atan(d.y,d.x)*.15915494309189535+.5,acos(clamp(d.z,-1,1))*.3183098861837907);
        float mip=roughness*probe.boxMaxMip.w;int lo=int(floor(mip)),hi=min(lo+1,int(probe.boxMaxMip.w));
        vec3 sampledRadiance;
        if(NativeEnvironmentSample(coordinate,mip,probe.boxMaxMip.w,probeParameters.z))
            sampledRadiance=textureLod(sampler2DArray(probePrefilterTexture,environmentLinearSampler),vec3(coordinate,float(i)),mip).rgb;
        else sampledRadiance=mix(SpatialMip(coordinate,lo,int(i)),SpatialMip(coordinate,hi,int(i)),fract(mip));
        sum+=sampledRadiance*probe.positionIntensity.w*
            (weight*ProbeVisibility(position,probe,i));
        weights+=weight;coverage+=edge;
    }
    if(probeParameters.y!=0u && weights>0 && coverage>=1 &&
        !any(isnan(envUV)) && !any(isinf(envUV)) && !isnan(mip) && !isinf(mip))
    {
        vec3 local=sum/weights;
        if(all(lessThanEqual(abs(local),vec3(2.1267646e37))))return local;
    }
    vec3 fallback;
    if(NativeEnvironmentSample(envUV,mip,frame.settings.w,probeParameters.z))
        fallback=textureLod(sampler2D(prefilterTexture,environmentLinearSampler),envUV,mip).rgb;
    else fallback=mix(LinearFloatTexture(prefilterTexture,envUV,lo,true),
        LinearFloatTexture(prefilterTexture,envUV,hi,true),fract(mip)).rgb;
    fallback*=frame.settings.y;
    return weights>0?mix(fallback,sum/weights,clamp(coverage,0,1)):fallback;
}
void main()
{
    ivec2 pixel=ivec2(gl_FragCoord.xy);
    float depth=texelFetch(sampler2D(depthTexture,clampSampler),pixel,0).r;
    if(depth>=1.0 || frame.settings.x<0.5) {outputColor=vec4(0);return;}
    vec4 surface=texelFetch(sampler2D(normalTexture,clampSampler),pixel,0);
    if(surface.z<0) {outputColor=vec4(0);return;}
    vec4 albedo=texelFetch(sampler2D(albedoTexture,clampSampler),pixel,0);
    vec4 world=frame.inverseViewProjection*vec4(uv*vec2(2,-2)+vec2(-1,1),depth,1);
    vec3 position=world.xyz/world.w;
    vec3 normal=DecodeNormal(surface.xy),view=normalize(frame.cameraPosition.xyz-position);
    float roughness=clamp(surface.z,0.01,1.0),nDotV=max(dot(normal,view),0.0);
    vec3 reflection=reflect(-view,normal);
    vec3 radiance=BlendProbeRadiance(position,reflection,roughness);
    vec2 brdf=LinearFloatTexture(brdfTexture,vec2(nDotV,roughness),0,false).rg;
    vec3 f0=mix(vec3(0.04),albedo.rgb,clamp(albedo.a,0.0,1.0));
    float ao=clamp(surface.w,0.0,1.0);
    // RGBA16F AO uses normalized UV and native bilinear filtering at its own resolution.
    if(frame.screen.z>0.5) ao*=clamp(textureLod(sampler2D(aoTexture,aoSampler),uv,0).r,0.0,1.0);
    vec3 result=radiance*(f0*brdf.x+brdf.y)*ao*frame.specularColor.rgb;
    outputColor=vec4(clamp(result,vec3(0),vec3(65504)),1);
}
