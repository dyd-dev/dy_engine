#ifndef RENDERER_ENABLE_AREA_LIGHTS
#define RENDERER_ENABLE_AREA_LIGHTS 1
#endif
#include "probe_visibility.hlsl"
struct Draw {column_major float4x4 model;column_major float4x4 normalMatrix;float4 baseColor;float4 emissive;float4 material;float4 optics;float4 attenuation;uint4 flags;};
StructuredBuffer<Draw> draws:register(t11);
cbuffer Frame:register(b12) {column_major float4x4 viewProjection;column_major float4x4 view;column_major float4x4 inverseViewProjection;float4 camera;float4 sh[9];float4 settings;float4 screen;float4 probePosition;float4 probeBoxMin;float4 probeBoxMax;float4 specularColor;};
cbuffer Index:register(b15) {uint4 drawIndex;};
Texture2D<float4> baseTexture:register(t0);
Texture2D<float4> materialTexture:register(t1);
Texture2D<float4> normalTexture:register(t2);
Texture2D<float4> aoTexture:register(t3);
Texture2D<float4> emissiveTexture:register(t4);
Texture2D<float4> opaqueTexture:register(t5);
Texture2D<float> depthTexture:register(t6);
Texture2D<float4> prefilterTexture:register(t7);
Texture2D<float> shadowTexture:register(t14);
Texture2D<float4> ltcMatrixTexture:register(t26);
Texture2D<float4> ltcAmplitudeTexture:register(t27);
Texture2D<float> nearestDepthTexture:register(t28);
SamplerState materialSampler:register(s8);
SamplerState screenSampler:register(s9);
SamplerState pointSampler:register(s10);
SamplerState environmentLinearSampler:register(s15);
struct Directional {float4 directionIntensity;float4 color;};
struct PointLight {float4 positionRange;float4 colorIntensity;};
struct SpotLight {float4 positionRange;float4 directionOuterCos;float4 colorIntensity;float4 cone;};
struct RendererRectAreaLight {float4 positionIntensity;float4 directionWidth;float4 upHeight;float4 color;};
struct RendererDiscAreaLight {float4 positionIntensity;float4 directionRadius;float4 up;float4 color;};
cbuffer Lights:register(b13) {float4 lightCameraPosition;float4 lightDirection;float4 lightColor;float4 ambient;float4 shadow;float4 pbr;
    float4 environment;float4 pointPosition;float4 pointColor;float4 counts;float4 areaCounts;
    Directional directionalLights[4];PointLight pointLights[16];SpotLight spotLights[16];
    RendererRectAreaLight rectAreaLights[4];RendererDiscAreaLight discAreaLights[4];float4 shadowLight;};
cbuffer ShadowMatrices:register(b16) {
    column_major float4x4 shadowViewProjection[128];float4 shadowAtlasRect[128];
    float4 shadowDirectionalViews[4];float4 shadowPointViews[16];float4 shadowSpotViews[16];
    float4 shadowDirectionalSplits[4];column_major float4x4 shadowCameraView;float4 shadowFilterParams;
    float4 shadowViewProjectionParams[128];float4 shadowSoftParams;
};
static const float PI=3.14159265359;
float3 TransparentPixelNormalizeOr(float3 value,float3 fallback)
{
    float scale=max(max(abs(value.x),abs(value.y)),abs(value.z));
    if(!(scale>0) || !all(isfinite(value)))return fallback;
    return normalize(value/scale);
}
struct ClusterLight {float4 positionRange;float4 colorIntensity;float4 directionOuterCos;float4 coneShadow;};
StructuredBuffer<ClusterLight> clusterLights:register(t19,space0);
StructuredBuffer<uint> clusterList:register(t20,space0);
cbuffer ClusterParameters:register(b21,space0) {column_major float4x4 clusterView;column_major float4x4 clusterProjection;
    column_major float4x4 clusterInverseProjection;float4 clusterDepth;uint4 clusterDimensions;uint4 clusterCounts;};
struct SpatialProbe {float4 positionIntensity;float4 boxMinFade;float4 boxMaxMip;float4 sh[9];};
StructuredBuffer<SpatialProbe> probes:register(t22);
Texture2DArray<float4> probePrefilterTexture:register(t23);
cbuffer ProbeParameters:register(b24) {uint4 probeParameters;};
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
uint ClusterBase(float3 position)
{
    if(clusterCounts.y==0)return 0xffffffffu;
    float4 viewPosition=mul(clusterView,float4(position,1)),clip=mul(clusterProjection,viewPosition);
    float distance=-viewPosition.z;
    if(distance<clusterDepth.x || distance>clusterDepth.y || clip.w<=0)return 0xffffffffu;
    float2 coordinate=clip.xy/clip.w*.5+.5;
    if(any(coordinate<0) || any(coordinate>=1))return 0xffffffffu;
    float slice=clusterCounts.z!=0?log(distance/clusterDepth.x)/clusterDepth.z:
        (distance-clusterDepth.x)/(clusterDepth.y-clusterDepth.x);
    uint3 index=min(uint3(coordinate*float2(clusterDimensions.xy),slice*float(clusterDimensions.z)),clusterDimensions.xyz-uint3(1,1,1));
    return (index.x+clusterDimensions.x*(index.y+clusterDimensions.y*index.z))*(clusterDimensions.w+1);
}
int2 DyShadowDimensions()
{
    uint width,height;shadowTexture.GetDimensions(width,height);return int2(width,height);
}
#define DY_SHADOW_PROJECTION(index) shadowViewProjectionParams[index]
#define DY_SHADOW_MATRIX(index) shadowViewProjection[index]
#define DY_SHADOW_TILE(index) shadowAtlasRect[index]
#define DY_SHADOW_DIMENSIONS DyShadowDimensions()
#define DY_SHADOW_READ(pixel) shadowTexture.Load(int3(pixel,0))
#define DY_SHADOW_FILTER shadowFilterParams
#define DY_SHADOW_SOFT shadowSoftParams
#include "shadow_filter.hlsl"

float ShadowView(int index,float3 position,float3 normal,float3 lightDir,float strength,float sourceRadius,int pointBase)
{
    float4 clip=mul(shadowViewProjection[index],float4(position,1));
    if(clip.w<=0)return 1;
    float3 ndc=clip.xyz/clip.w;float2 coordinate=ndc.xy*.5+.5;coordinate.y=1-coordinate.y;
    if(any(coordinate<0) || any(coordinate>1) || ndc.z<0 || ndc.z>1)return 1;
    float4 tile=shadowAtlasRect[index];uint width,height;shadowTexture.GetDimensions(width,height);float2 texel=1.0/float2(width,height);
    float2 minimum=tile.xy+texel*.5,maximum=tile.xy+tile.zw-texel*.5;
    float2 localCoordinate=coordinate;
    coordinate=tile.xy+coordinate*tile.zw;
    float angle=1-max(dot(normal,lightDir),0);
    float receiver=ndc.z-max(shadow.x,shadow.y*angle)-shadow.z*angle;
    if(sourceRadius>0 && shadowSoftParams.x>0)
        return 1-clamp(strength,0.0,1.0)*(1-DyShadowPcss(index,pointBase,localCoordinate,receiver,clip.w,lightDir,sourceRadius));
    int extent=int(ceil(min(shadow.w,shadowFilterParams.z)));
    float visible=0,samples=float((2*extent+1)*(2*extent+1));
    for(int y=-extent;y<=extent;++y)for(int x=-extent;x<=extent;++x)
        visible+=receiver<=shadowTexture.SampleLevel(pointSampler,clamp(coordinate+float2(x,y)*texel,minimum,maximum),0)?1:0;
    return 1-saturate(strength)*(1-visible/max(samples,1));
}
float Visibility(int type,int lightIndex,float3 position,float3 normal,float3 lightDir,uint flags)
{
    if((flags&32u)==0u || dot(normal,lightDir)<=0)return 1;
    float4 views=type==1?shadowDirectionalViews[lightIndex]:(type==2?shadowPointViews[lightIndex]:shadowSpotViews[lightIndex]);
    int count=int(views.y+.5);if(count==0)return 1;
    int local=0;float depth=0;
    if(type==1)
    {
        depth=-mul(shadowCameraView,float4(position,1)).z;
        float lastSplit=shadowDirectionalSplits[lightIndex][count-1];
        if(lastSplit>0 && depth>lastSplit)return 1;
        while(local<count-1 && depth>shadowDirectionalSplits[lightIndex][local])++local;
    }
    if(type==2)
    {
        float3 direction=-lightDir,magnitude=abs(direction);
        if(magnitude.x>=magnitude.y && magnitude.x>=magnitude.z)local=direction.x>=0?0:1;
        else if(magnitude.y>=magnitude.z)local=direction.y>=0?2:3;
        else local=direction.z>=0?4:5;
    }
    int index=int(views.x+.5)+local;float visibility=ShadowView(index,position,normal,lightDir,views.z,views.w,type==2?int(views.x+.5):-1);
    if(type==1 && local<count-1)
    {
        float previous=local==0?shadowFilterParams.w:shadowDirectionalSplits[lightIndex][local-1];
        float split=shadowDirectionalSplits[lightIndex][local],blend=smoothstep(lerp(previous,split,.9),split,depth);
        if(blend>0)visibility=lerp(visibility,ShadowView(index+1,position,normal,lightDir,views.z,views.w,type==2?int(views.x+.5):-1),blend);
    }
    return visibility;
}
float3 Fresnel(float cosine,float3 f0) {return f0+(1-f0)*pow(saturate(1-cosine),5);}
float2 EnvUV(float3 d)
{
    float c=cos(settings.z),s=sin(settings.z);d.xy=float2(c*d.x+s*d.y,-s*d.x+c*d.y);
    return float2(atan2(d.y,d.x)*(.5/PI)+.5,acos(clamp(d.z,-1,1))/PI);
}
float3 EnvMip(float2 coordinate,int mip)
{
    uint width,height,levels;prefilterTexture.GetDimensions(uint(mip),width,height,levels);int2 size=int2(width,height);
    float2 pixel=coordinate*float2(size)-.5,w=frac(pixel);int2 a=int2(floor(pixel)),b=a+1;
    int2 wrappedX=DyWrapBilinearX(a.x,b.x,size.x);
    a.x=wrappedX.x;b.x=wrappedX.y;
    a.y=clamp(a.y,0,size.y-1);b.y=clamp(b.y,0,size.y-1);
    return lerp(lerp(prefilterTexture.Load(int3(a,mip)),prefilterTexture.Load(int3(b.x,a.y,mip)),w.x),
        lerp(prefilterTexture.Load(int3(a.x,b.y,mip)),prefilterTexture.Load(int3(b,mip)),w.x),w.y).rgb;
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
    return drawIndex.z==1u && enabled==1u && all(coordinate>=0) && all(coordinate<=1)
        && mip>=0 && mip<=maximumMip && maximumMip<=32;
}
float ProbeVisibility(float3 position,SpatialProbe probe,uint index)
{
    return SpatialProbeVisibility(position,probe.positionIntensity.xyz,probe.sh[0].w,probe.sh[1].w,
        probeParameters.x+index,probePrefilterTexture);
}
float3 Radiance(float3 position,float3 d,float roughness)
{
    float mip=roughness*settings.w;int lo=int(floor(mip)),hi=min(lo+1,int(settings.w));
    float2 coordinate=EnvUV(ProbeDirection(position,d));
    float3 sum=0;float weights=0,coverage=0;
    for(uint i=0;i<probeParameters.x;++i)
    {
        float edge;float weight=ProbeWeight(position,probes[i],edge);if(weight<=0)continue;
        float3 direction=BoxDirection(position,d,probes[i].positionIntensity.xyz,probes[i].boxMinFade.xyz,probes[i].boxMaxMip.xyz);
        float2 localUV=float2(atan2(direction.y,direction.x)*.15915494309189535+.5,acos(clamp(direction.z,-1,1))*.3183098861837907);
        float localMip=roughness*probes[i].boxMaxMip.w;int a=int(floor(localMip)),b=min(a+1,int(probes[i].boxMaxMip.w));
        float3 sampledRadiance;
        if(NativeEnvironmentSample(localUV,localMip,probes[i].boxMaxMip.w,probeParameters.z))
            sampledRadiance=probePrefilterTexture.SampleLevel(environmentLinearSampler,float3(localUV,float(i)),localMip).rgb;
        else sampledRadiance=lerp(SpatialMip(localUV,a,int(i)),SpatialMip(localUV,b,int(i)),frac(localMip));
        sum+=sampledRadiance*probes[i].positionIntensity.w*
            (weight*ProbeVisibility(position,probes[i],i));
        weights+=weight;coverage+=edge;
    }
    if(probeParameters.y!=0u && weights>0 && coverage>=1 && all(isfinite(coordinate)) && isfinite(mip))
    {
        float3 local=sum/weights;
        if(all(abs(local)<=2.1267646e37))return local;
    }
    float3 fallback;
    if(NativeEnvironmentSample(coordinate,mip,settings.w,probeParameters.z))
        fallback=prefilterTexture.SampleLevel(environmentLinearSampler,coordinate,mip).rgb;
    else fallback=lerp(EnvMip(coordinate,lo),EnvMip(coordinate,hi),frac(mip));
    fallback*=settings.y;
    return weights>0?lerp(fallback,sum/weights,saturate(coverage)):fallback;
}
float3 Irradiance(float3 position,float3 n)
{
    float3 worldNormal=n;
    float c=cos(settings.z),s=sin(settings.z);n.xy=float2(c*n.x+s*n.y,-s*n.x+c*n.y);
    float3 irradiance=(sh[0].rgb*.28209479+sh[1].rgb*(.48860251*n.y)+sh[2].rgb*(.48860251*n.z)+
        sh[3].rgb*(.48860251*n.x)+sh[4].rgb*(1.09254843*n.x*n.y)+sh[5].rgb*(1.09254843*n.y*n.z)+
        sh[6].rgb*(.31539156*(3*n.z*n.z-1))+sh[7].rgb*(1.09254843*n.x*n.z)+
        sh[8].rgb*(.54627422*(n.x*n.x-n.y*n.y)))*settings.y;
    float3 sum=0;float weights=0,coverage=0;
    for(uint i=0;i<probeParameters.x;++i)
    {
        float edge;float weight=ProbeWeight(position,probes[i],edge);if(weight<=0)continue;
        sum+=ProbeIrradiance(probes[i],worldNormal)*(weight*ProbeVisibility(position,probes[i],i));
        weights+=weight;coverage+=edge;
    }
    if(weights>0)irradiance=lerp(irradiance,sum/weights,saturate(coverage));
    return max(irradiance,0)/PI;
}
float3 Direct(float3 n,float3 v,float3 l,float3 energy,float3 albedo,float3 f0,float roughness,float diffuse)
{
    float nl=max(dot(n,l),0),nv=max(dot(n,v),.001);if(nl<=0 || dot(v+l,v+l)<1e-8)return 0;
    float3 h=normalize(v+l);float nh=saturate(dot(n,h)),a=roughness*roughness,a2=a*a;
    float3 crossNH=cross(n,h);float denominator=dot(crossNH,crossNH)+nh*nh*a2;
    float d=a2/(PI*denominator*denominator);float k=pow(roughness+1,2)*.125;
    float g=nv/(nv*(1-k)+k)*nl/(nl*(1-k)+k);float3 f=Fresnel(max(dot(h,v),0),f0);
    return ((1-f)*albedo*(diffuse/PI)+d*g*f/max(4*nv*nl,.0001))*energy*nl;
}
float Falloff(float distance,float range)
{
    if(range<=0 || distance>=range)return 0;float x=distance/range;
    return pow(max(1-x*x*x*x,0),2)/max(distance*distance,.0001);
}
struct Input {float4 position:SV_Position;float2 uv:TEXCOORD0;float3 worldPosition:TEXCOORD1;float3 worldNormal:TEXCOORD2;float4 worldTangent:TEXCOORD3;};
struct Output {float4 color:SV_Target0;float4 reactive:SV_Target1;};
#if RENDERER_ENABLE_AREA_LIGHTS
// LTC 적분과 표 배치는 Heitz 외 SIGGRAPH 2016 및 Assets/ltc/LICENSE를 따른다.
inline float LtcEdgeIntegral(float3 a,float3 b)
{
    float3 edge=cross(a,b);
    float sine=length(edge),cosine=clamp(dot(a,b),-1.0,1.0);
    return edge.z*(atan2(sine,cosine)/max(sine,1e-12))*0.15915494309189535;
}
inline float3 LtcTransform(float3 offset,float3 tangent,float3 bitangent,float3 normal,float4 inverseMatrix)
{
    float3 local=float3(dot(offset,tangent),dot(offset,bitangent),dot(offset,normal));
    return float3(inverseMatrix.x*local.x+inverseMatrix.z*local.z,local.y,
        inverseMatrix.y*local.x+inverseMatrix.w*local.z);
}
inline float LtcPolygonIntegral(float3 normal,float3 viewDir,float3 position,
    float4 inverseMatrix,float3 points[16],int count)
{
    float3 tangent=viewDir-normal*dot(normal,viewDir);
    if(dot(tangent,tangent)<1e-8)
        tangent=cross(abs(normal.z)<.999?float3(0,0,1):float3(0,1,0),normal);
    tangent=normalize(tangent);
    float3 bitangent=cross(normal,tangent);
    float3 previous=LtcTransform(points[count-1]-position,tangent,bitangent,normal,inverseMatrix);
    float3 first=float3(0,0,0),last=float3(0,0,0);bool haveVertex=false;float sum=0;
    for(int edge=0;edge<count;++edge)
    {
        float3 current=LtcTransform(points[edge]-position,tangent,bitangent,normal,inverseMatrix);
        bool previousInside=previous.z>=0,currentInside=current.z>=0;
        // z=0 반구로 다각형을 자르되 정점 배열 대신 경계 순서대로 적분한다.
        for(int candidate=0;candidate<2;++candidate)
        {
            float3 clippedPoint;
            if(candidate==0)
            {
                if(previousInside==currentInside)continue;
                clippedPoint=lerp(previous,current,clamp(previous.z/(previous.z-current.z),0.0,1.0));
                clippedPoint.z=0;
            }
            else
            {
                if(!currentInside)continue;
                clippedPoint=current;
            }
            float squared=dot(clippedPoint,clippedPoint);
            if(squared<=1e-20)continue;
            clippedPoint*=rsqrt(squared);
            if(haveVertex)sum+=LtcEdgeIntegral(last,clippedPoint);else {first=clippedPoint;haveVertex=true;}
            last=clippedPoint;
        }
        previous=current;
    }
    if(haveVertex)sum+=LtcEdgeIntegral(last,first);
    return clamp(abs(sum),0.0,1.0);
}
inline float3 EvaluateLtcPolygon(float3 normal,float3 viewDir,float3 position,float3 points[16],int count,
    float3 albedo,float3 f0,float diffuseFactor,float roughness,float3 radiance)
{
    float ndotv=clamp(dot(normal,viewDir),0.0,1.0);
    if(ndotv<=0)return float3(0,0,0);
    float2 coordinate=(float2(clamp(roughness,0.0,1.0),sqrt(1-ndotv))*63.0+.5)/64.0;
    float4 inverseMatrix=ltcMatrixTexture.SampleLevel(materialSampler,coordinate,0);
    float4 amplitude=ltcAmplitudeTexture.SampleLevel(materialSampler,coordinate,0);
    float specular=LtcPolygonIntegral(normal,viewDir,position,inverseMatrix,points,count);
    float diffuse=LtcPolygonIntegral(normal,viewDir,position,float4(1,0,0,1),points,count);
    float3 specularWeight=f0*max(amplitude.x,0.0)+(float3(1,1,1)-f0)*max(amplitude.y,0.0);
    float3 diffuseWeight=(float3(1,1,1)-Fresnel(ndotv,f0))*diffuseFactor;
    // 정규화된 cosine 적분이므로 diffuse에 1/pi나 거리 감쇠를 다시 곱하지 않는다.
    return radiance*(specularWeight*specular+diffuseWeight*albedo*diffuse);
}
inline float3 EvaluateRectAreaLight(RendererRectAreaLight light,float3 normal,float3 viewDir,
    float3 albedo,float3 f0,float diffuseFactor,float roughness,float3 worldPosition)
{
    float3 direction=normalize(light.directionWidth.xyz);
    if(light.directionWidth.w<=0 || light.upHeight.w<=0 || dot(direction,worldPosition-light.positionIntensity.xyz)<=0)
        return float3(0,0,0);
    float3 right=normalize(cross(direction,light.upHeight.xyz));
    float3 up=normalize(cross(right,direction));
    float3 x=right*(light.directionWidth.w*.5),y=up*(light.upHeight.w*.5),center=light.positionIntensity.xyz;
    float3 points[16];
    points[0]=center-x-y;points[1]=center+x-y;points[2]=center+x+y;points[3]=center-x+y;
    return EvaluateLtcPolygon(normal,viewDir,worldPosition,points,4,albedo,f0,diffuseFactor,roughness,
        light.color.rgb*max(light.positionIntensity.w,0.0));
}
inline float3 EvaluateDiscAreaLight(RendererDiscAreaLight light,float3 normal,float3 viewDir,
    float3 albedo,float3 f0,float diffuseFactor,float roughness,float3 worldPosition)
{
    float3 direction=normalize(light.directionRadius.xyz);
    if(light.directionRadius.w<=0 || dot(direction,worldPosition-light.positionIntensity.xyz)<=0)return float3(0,0,0);
    float3 right=normalize(cross(direction,light.up.xyz));
    float3 up=normalize(cross(right,direction));
    float3 points[16];
    // 원판 경계를 16각형으로 근사한 뒤 면 전체를 LTC로 적분한다.
    for(int corner=0;corner<16;++corner)
    {
        float angle=float(corner)*0.39269908169872415;
        points[corner]=light.positionIntensity.xyz+(right*cos(angle)+up*sin(angle))*light.directionRadius.w;
    }
    return EvaluateLtcPolygon(normal,viewDir,worldPosition,points,16,albedo,f0,diffuseFactor,roughness,
        light.color.rgb*max(light.positionIntensity.w,0.0));
}
#endif

[earlydepthstencil]
Output main(Input input,bool frontFacing:SV_IsFrontFace)
{
    // Preserve material gradients while all lanes still participate.
    float2 uvDx=ddx(input.uv),uvDy=ddy(input.uv);
    float2 screenUV=input.position.xy*screen.xy;
    float opaqueDepth=depthTexture.Load(int3(int2(input.position.xy),0));
    bool excludedLayer=false;
    if(drawIndex.y!=0u)
    {
        float nearestDepth=nearestDepthTexture.Load(int3(int2(input.position.xy),0));
        uint currentBits=asuint(input.position.z),nearestBits=asuint(nearestDepth);
        // 같은 정점 셰이더의 D32 결과를 한 ULP까지만 비교하고 거리 비례 오차는 넣지 않는다.
        bool frontLayer=max(currentBits,nearestBits)-min(currentBits,nearestBits)<=1u;
        excludedLayer=drawIndex.y==1u?frontLayer:!frontLayer;
    }
    Draw draw=draws[drawIndex.x];uint flags=draw.flags.x;
    float3 geometricNormal=float3(0,0,1),n=geometricNormal,v=geometricNormal;
    float normalVariance=0;
    // These flags are uniform for the draw; debug/unlit materials need no normal footprint.
    if((flags&(64u|128u))==0u)
    {
        geometricNormal=TransparentPixelNormalizeOr(input.worldNormal,float3(0,0,1));n=geometricNormal;
        v=TransparentPixelNormalizeOr(camera.xyz-input.worldPosition,geometricNormal);
        if((flags&4u)!=0u)
        {
            float3 fallback=normalize(cross(abs(n.z)<.99?float3(0,0,1):float3(0,1,0),n));
            float3 t=TransparentPixelNormalizeOr(input.worldTangent.xyz-n*dot(n,input.worldTangent.xyz),fallback),b=cross(n,t)*input.worldTangent.w;
            float3 mapped=normalTexture.Sample(materialSampler,input.uv).xyz*2-1;
            mapped.xy*=draw.material.z;mapped.y*=(draw.emissive.w<0?-1.0:1.0);
            n=TransparentPixelNormalizeOr(t*mapped.x+b*mapped.y+n*mapped.z,n);
        }
        // Keep mapped-normal derivatives before discard or visible-side selection.
        float3 normalDx=ddx(n),normalDy=ddy(n);
        normalVariance=0.25*(dot(normalDx,normalDx)+dot(normalDy,normalDy));
    }
    if(input.position.z>opaqueDepth || excludedLayer)discard;
    float4 base=draw.baseColor;
    if((flags&1u)!=0u)base*=baseTexture.SampleGrad(materialSampler,input.uv,uvDx,uvDy);
    float2 mr=draw.material.xy;if((flags&2u)!=0u)mr*=materialTexture.SampleGrad(materialSampler,input.uv,uvDx,uvDy).bg;
    float metal=saturate(mr.x),rough=clamp(mr.y,.04,1),transmission=saturate(draw.optics.x)*(1-metal);
    if((flags&64u)!=0u)
    {
        float coverage=lerp(saturate(base.a),1,transmission);
        Output output;output.color=float4(draw.baseColor.rgb*coverage,coverage);output.reactive=coverage;return output;
    }
    if((flags&128u)!=0u)
    {
        float coverage=saturate(base.a);
        Output output;output.color=float4(clamp(base.rgb,0,65504)*coverage,coverage);output.reactive=coverage;return output;
    }
    float filteredAlphaSquared=min(1.0,rough*rough*rough*rough+min(2.0*normalVariance,0.18));
    float specularRoughness=sqrt(sqrt(filteredAlphaSquared));
    if(transmission>0)
    {
        // Explicit-thickness thin-wall approximation: either visible mesh side
        // is an air-to-material interface, not an independently traced exit.
        if(dot(geometricNormal,v)<0) {geometricNormal=-geometricNormal;n=-n;}
        if(dot(n,v)<=0)n=geometricNormal;
    }
    else if(draw.flags.y!=0u && !frontFacing)n=-n;
    float ior=draw.optics.y,dielectric=pow((ior-1)/(ior+1),2),nv=max(dot(n,v),0);
    float3 transmissionRay=transmission>0?refract(-v,n,1/ior):float3(0,0,0);
    bool totalInternalReflection=transmission>0 && dot(transmissionRay,transmissionRay)<1e-6;
    float3 f0=totalInternalReflection?float3(1,1,1):lerp(float3(dielectric,dielectric,dielectric),base.rgb,metal),f=Fresnel(nv,f0);
    float ao=1;if((flags&8u)!=0u)ao=lerp(1,aoTexture.Sample(materialSampler,input.uv).r,draw.material.w);
    float3 lit=(1-f)*base.rgb*(1-metal)*(1-transmission)*Irradiance(input.worldPosition,n)*ao;
    if((uint(screen.w+.5)&2u)!=0u)lit+=Radiance(input.worldPosition,reflect(-v,n),specularRoughness)*f*specularColor.rgb;
    for(int i=0;i<clamp(int(counts.x+.5),0,4);++i)
    {
        float3 l=normalize(directionalLights[i].directionIntensity.xyz);
        float visibility=Visibility(1,i,input.worldPosition,n,l,flags);
        lit+=Direct(n,v,l,directionalLights[i].color.rgb*directionalLights[i].directionIntensity.w*visibility,
            base.rgb,f0,specularRoughness,(1-metal)*(1-transmission));
    }
    uint clusterBase=ClusterBase(input.worldPosition),listed=0xffffffffu;
    if(clusterBase!=0xffffffffu)listed=clusterList[clusterBase];
    uint localCount=listed==0xffffffffu?clusterCounts.x:listed;
    for(uint item=0u;item<localCount;++item)
    {
        uint lightIndex=listed==0xffffffffu?item:clusterList[clusterBase+1u+item];
        ClusterLight light=clusterLights[lightIndex];
        float3 delta=light.positionRange.xyz-input.worldPosition;float distance=length(delta);
        if(distance<=1e-4 || distance>=light.positionRange.w)continue;
        float3 l=delta/distance;
        int type=int(light.coneShadow.y+.5),shadowIndex=int(light.coneShadow.z);
        float cone=1;
        if(type==3)cone=smoothstep(light.directionOuterCos.w,max(light.coneShadow.x,light.directionOuterCos.w+1e-4),
            dot(-l,light.directionOuterCos.xyz));
        float visibility=shadowIndex>=0?Visibility(type,shadowIndex,input.worldPosition,n,l,flags):1;
        lit+=Direct(n,v,l,light.colorIntensity.rgb*light.colorIntensity.w*Falloff(distance,light.positionRange.w)*cone*visibility,
            base.rgb,f0,specularRoughness,(1-metal)*(1-transmission));
    }
#if RENDERER_ENABLE_AREA_LIGHTS
    for(int i=0;i<clamp(int(areaCounts.x+.5),0,4);++i)
        lit+=EvaluateRectAreaLight(rectAreaLights[i],n,v,base.rgb,f0,(1-metal)*(1-transmission),specularRoughness,input.worldPosition);
    for(int i=0;i<clamp(int(areaCounts.y+.5),0,4);++i)
        lit+=EvaluateDiscAreaLight(discAreaLights[i],n,v,base.rgb,f0,(1-metal)*(1-transmission),specularRoughness,input.worldPosition);
#endif
    float3 refracted=0;
    if(transmission>0 && !totalInternalReflection)
    {
        float3 ray=transmissionRay;
        float path=draw.optics.z/max(abs(dot(n,ray)),.1);float4 exitClip=mul(viewProjection,float4(input.worldPosition+ray*path,1));
        float2 target=exitClip.xy/max(exitClip.w,1e-5)*float2(.5,-.5)+.5;
        float exitDepth=exitClip.z/max(exitClip.w,1e-5);
        bool onScreen=exitClip.w>0 && exitDepth>=0 && exitDepth<=1 && all(target>=0) && all(target<=1);
        if(onScreen && depthTexture.SampleLevel(pointSampler,target,0)<exitDepth)target=screenUV;
        refracted=onScreen?opaqueTexture.SampleLevel(screenSampler,target,0).rgb:Radiance(input.worldPosition,ray,rough);
        float3 absorption=draw.optics.w>0?pow(clamp(draw.attenuation.rgb,.0001,1),float3(path/draw.optics.w,path/draw.optics.w,path/draw.optics.w)):float3(1,1,1);
        refracted*=absorption*base.rgb*(1-f)*transmission;
    }
    float3 emissive=draw.emissive.rgb;if((flags&16u)!=0u)emissive*=emissiveTexture.Sample(materialSampler,input.uv).rgb;
    float coverage=lerp(saturate(base.a),1,transmission);
    float3 color=counts.w>.5?lit+refracted+emissive:base.rgb+emissive;
    Output output;output.color=float4(clamp(color,0,65504)*coverage,coverage);output.reactive=coverage;return output;
}
