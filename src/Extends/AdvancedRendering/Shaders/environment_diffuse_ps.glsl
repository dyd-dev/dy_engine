#version 450
#extension GL_GOOGLE_include_directive : require
#include "probe_visibility.glsl"
layout(location=0) in vec2 uv;
layout(location=0) out vec4 outputColor;
layout(set=0,binding=0) uniform texture2D depthTexture;
layout(set=0,binding=1) uniform texture2D normalTexture;
layout(set=0,binding=3) uniform texture2D aoTexture;
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
float ProbeVisibility(vec3 position,SpatialProbe probe,uint index)
{
    return SpatialProbeVisibility(position,probe.positionIntensity.xyz,probe.sh[0].w,probe.sh[1].w,
        probeParameters.x+index,probePrefilterTexture,clampSampler,environmentLinearSampler,probeParameters.w);
}
float ProbeWeight(vec3 position,SpatialProbe probe,out float coverage)
{
    vec3 margin=min(position-probe.boxMinFade.xyz,probe.boxMaxMip.xyz-position);
    coverage=clamp(min(margin.x,min(margin.y,margin.z))/probe.boxMinFade.w,0,1);
    vec3 delta=position-probe.positionIntensity.xyz,extent=probe.boxMaxMip.xyz-probe.boxMinFade.xyz;
    return coverage/max(dot(delta,delta),max(dot(extent,extent)*.0001,1e-6));
}
vec3 ProbeIrradiance(SpatialProbe probe,vec3 n)
{
    return (probe.sh[0].rgb*.28209479177387814+probe.sh[1].rgb*(.4886025119029199*n.y)+
        probe.sh[2].rgb*(.4886025119029199*n.z)+probe.sh[3].rgb*(.4886025119029199*n.x)+
        probe.sh[4].rgb*(1.0925484305920792*n.x*n.y)+probe.sh[5].rgb*(1.0925484305920792*n.y*n.z)+
        probe.sh[6].rgb*(.31539156525252005*(3*n.z*n.z-1))+probe.sh[7].rgb*(1.0925484305920792*n.x*n.z)+
        probe.sh[8].rgb*(.5462742152960396*(n.x*n.x-n.y*n.y)))*probe.positionIntensity.w;
}
vec3 DecodeNormal(vec2 oct)
{
    oct=oct*2.0-1.0;
    vec3 n=vec3(oct,1.0-abs(oct.x)-abs(oct.y));
    if(n.z<0.0) n.xy=(1.0-abs(n.yx))*mix(vec2(-1),vec2(1),greaterThanEqual(n.xy,vec2(0)));
    return normalize(n);
}
void main()
{
    ivec2 pixel=ivec2(gl_FragCoord.xy);
    float depth=texelFetch(sampler2D(depthTexture,clampSampler),pixel,0).r;
    if(depth>=1.0) {outputColor=vec4(0);return;}
    vec4 surface=texelFetch(sampler2D(normalTexture,clampSampler),pixel,0);
    if(surface.z<0) {outputColor=vec4(0);return;}
    vec3 n=DecodeNormal(surface.xy);
    vec3 worldNormal=n;
    float c=cos(frame.settings.z),s=sin(frame.settings.z);
    n.xy=mat2(c,-s,s,c)*n.xy;
    vec3 irradiance=frame.sh[0].rgb*0.28209479177387814+
        frame.sh[1].rgb*(0.4886025119029199*n.y)+frame.sh[2].rgb*(0.4886025119029199*n.z)+
        frame.sh[3].rgb*(0.4886025119029199*n.x)+frame.sh[4].rgb*(1.0925484305920792*n.x*n.y)+
        frame.sh[5].rgb*(1.0925484305920792*n.y*n.z)+frame.sh[6].rgb*(0.31539156525252005*(3*n.z*n.z-1))+
        frame.sh[7].rgb*(1.0925484305920792*n.x*n.z)+frame.sh[8].rgb*(0.5462742152960396*(n.x*n.x-n.y*n.y));
    irradiance*=frame.settings.y;
    if(probeParameters.x>0u)
    {
        vec4 world=frame.inverseViewProjection*vec4(uv*vec2(2,-2)+vec2(-1,1),depth,1);
        vec3 position=world.xyz/world.w,sum=vec3(0);float weights=0,coverage=0;
        for(uint i=0u;i<probeParameters.x;++i)
        {
            float edge;float weight=ProbeWeight(position,probes[i],edge);
            if(weight<=0)continue;
            sum+=ProbeIrradiance(probes[i],worldNormal)*(weight*ProbeVisibility(position,probes[i],i));
            weights+=weight;coverage+=edge;
        }
        if(weights>0)irradiance=mix(irradiance,sum/weights,clamp(coverage,0,1));
    }
    float ao=clamp(surface.w,0.0,1.0);
    if(frame.screen.z>0.5) ao*=clamp(textureLod(sampler2D(aoTexture,aoSampler),uv,0).r,0.0,1.0);
    outputColor=vec4(clamp(max(irradiance,vec3(0))*(0.3183098861837907*ao),vec3(0),vec3(65504)),1);
}
