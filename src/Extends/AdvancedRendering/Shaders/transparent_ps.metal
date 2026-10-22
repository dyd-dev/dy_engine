#ifndef RENDERER_ENABLE_AREA_LIGHTS
#define RENDERER_ENABLE_AREA_LIGHTS 1
#endif
#include <metal_stdlib>
using namespace metal;
#ifndef TRANSPARENT_FRAGMENT_ENTRY
#define TRANSPARENT_FRAGMENT_ENTRY transparentFragment
#endif
#include "probe_visibility.metal"
struct TransparentDraw {float4x4 model;float4x4 normalMatrix;float4 baseColor;float4 emissive;float4 material;float4 optics;float4 attenuation;uint4 flags;};
struct TransparentFrame {float4x4 viewProjection;float4x4 view;float4x4 inverseViewProjection;float4 camera;float4 sh[9];float4 settings;float4 screen;float4 probePosition;float4 probeBoxMin;float4 probeBoxMax;float4 specularColor;};
struct TransparentVertex {float4 position [[position,invariant]];float2 uv;float3 worldPosition;float3 worldNormal;float4 worldTangent;};
struct TransparentOutput {float4 color [[color(0)]];float4 reactive [[color(1)]];};
struct TransparentDirectional {float4 directionIntensity;float4 color;};
struct TransparentPoint {float4 positionRange;float4 colorIntensity;};
struct TransparentSpot {float4 positionRange;float4 directionOuterCos;float4 colorIntensity;float4 cone;};
struct RendererRectAreaLight {float4 positionIntensity;float4 directionWidth;float4 upHeight;float4 color;};
struct RendererDiscAreaLight {float4 positionIntensity;float4 directionRadius;float4 up;float4 color;};
struct TransparentLights {float4 cameraPosition;float4 direction;float4 color;float4 ambient;float4 shadow;float4 pbr;
    float4 environment;float4 pointPosition;float4 pointColor;float4 counts;float4 areaCounts;
    TransparentDirectional directional[4];TransparentPoint point[16];TransparentSpot spot[16];
    RendererRectAreaLight rectAreaLights[4];RendererDiscAreaLight discAreaLights[4];float4 shadowLight;};
struct TransparentShadows {
    float4x4 lightViewProjectionMatrix[128];float4 atlasRect[128];
    float4 directionalViews[4];float4 pointViews[16];float4 spotViews[16];
    float4 directionalSplits[4];float4x4 cameraView;float4 filterParams;
    float4 viewProjectionParams[128];float4 softShadowParams;
};
struct ClusterLight {float4 positionRange;float4 colorIntensity;float4 directionOuterCos;float4 coneShadow;};
struct ClusterParameters {float4x4 view;float4x4 projection;float4x4 inverseProjection;float4 depth;uint4 dimensions;uint4 counts;};
struct TransparentSpatialProbe {float4 positionIntensity;float4 boxMinFade;float4 boxMaxMip;float4 sh[9];};
float TransparentProbeWeight(float3 position,const device TransparentSpatialProbe& probe,thread float& coverage)
{
    float3 margin=min(position-probe.boxMinFade.xyz,probe.boxMaxMip.xyz-position);
    coverage=clamp(min(margin.x,min(margin.y,margin.z))/probe.boxMinFade.w,0.0f,1.0f);
    float3 delta=position-probe.positionIntensity.xyz,extent=probe.boxMaxMip.xyz-probe.boxMinFade.xyz;
    return coverage/max(dot(delta,delta),max(dot(extent,extent)*.0001f,1e-6f));
}
float3 TransparentProbeIrradiance(const device TransparentSpatialProbe& probe,float3 n)
{
    return (probe.sh[0].rgb*.28209479177387814f+probe.sh[1].rgb*(.4886025119029199f*n.y)+
        probe.sh[2].rgb*(.4886025119029199f*n.z)+probe.sh[3].rgb*(.4886025119029199f*n.x)+
        probe.sh[4].rgb*(1.0925484305920792f*n.x*n.y)+probe.sh[5].rgb*(1.0925484305920792f*n.y*n.z)+
        probe.sh[6].rgb*(.31539156525252005f*(3*n.z*n.z-1))+probe.sh[7].rgb*(1.0925484305920792f*n.x*n.z)+
        probe.sh[8].rgb*(.5462742152960396f*(n.x*n.x-n.y*n.y)))*probe.positionIntensity.w;
}
inline uint ClusterBase(float3 position,constant ClusterParameters& clusters)
{
    if(clusters.counts.y==0u)return 0xffffffffu;
    float4 viewPosition=clusters.view*float4(position,1),clip=clusters.projection*viewPosition;
    float distance=-viewPosition.z;
    if(distance<clusters.depth.x || distance>clusters.depth.y || clip.w<=0)return 0xffffffffu;
    float2 coordinate=clip.xy/clip.w*.5f+.5f;
    if(any(coordinate<0) || any(coordinate>=1))return 0xffffffffu;
    float slice=clusters.counts.z!=0u?log(distance/clusters.depth.x)/clusters.depth.z:
        (distance-clusters.depth.x)/(clusters.depth.y-clusters.depth.x);
    uint3 index=min(uint3(coordinate*float2(clusters.dimensions.xy),slice*float(clusters.dimensions.z)),clusters.dimensions.xyz-uint3(1));
    return (index.x+clusters.dimensions.x*(index.y+clusters.dimensions.y*index.z))*(clusters.dimensions.w+1u);
}
constant float TransparentPI=3.14159265359f;
float3 TransparentPixelNormalizeOr(float3 value,float3 fallback)
{
    float scale=max(max(abs(value.x),abs(value.y)),abs(value.z));
    if(!(scale>0) || !all(isfinite(value)))return fallback;
    return normalize(value/scale);
}
#define DY_SHADOW_STRUCT TransparentShadows
#define DY_SHADOW_TEXTURE depth2d<float>
#define DY_SHADOW_PROJECTION(index) shadowData.viewProjectionParams[index]
#define DY_SHADOW_MATRIX(index) shadowData.lightViewProjectionMatrix[index]
#define DY_SHADOW_TILE(index) shadowData.atlasRect[index]
#define DY_SHADOW_DIMENSIONS int2(shadowImage.get_width(),shadowImage.get_height())
#define DY_SHADOW_READ(pixel) shadowImage.read(uint2(pixel))
#define DY_SHADOW_FILTER shadowData.filterParams
#define DY_SHADOW_SOFT shadowData.softShadowParams
#include "shadow_filter.metal"

float TransparentShadowView(int index,float3 position,float3 normal,float3 lightDir,float strength,float sourceRadius,int pointBase,
    constant TransparentLights& lights,constant TransparentShadows& shadows,depth2d<float> image,sampler imageSampler)
{
    float4 clip=shadows.lightViewProjectionMatrix[index]*float4(position,1);
    if(clip.w<=0)return 1;
    float3 ndc=clip.xyz/clip.w;float2 coordinate=ndc.xy*.5f+.5f;coordinate.y=1-coordinate.y;
    if(any(coordinate<0) || any(coordinate>1) || ndc.z<0 || ndc.z>1)return 1;
    float4 tile=shadows.atlasRect[index];float2 texel=1.0f/float2(image.get_width(),image.get_height());
    float2 minimum=tile.xy+texel*.5f,maximum=tile.xy+tile.zw-texel*.5f;
    float2 localCoordinate=coordinate;
    coordinate=tile.xy+coordinate*tile.zw;
    float angle=1-max(dot(normal,lightDir),0.0f);
    float receiver=ndc.z-max(lights.shadow.x,lights.shadow.y*angle)-lights.shadow.z*angle;
    if(sourceRadius>0 && shadows.softShadowParams.x>0)
        return 1-clamp(strength,0.0,1.0)*(1-DyShadowPcss(index,pointBase,localCoordinate,receiver,clip.w,lightDir,sourceRadius,shadows,image));
    int extent=int(ceil(min(lights.shadow.w,shadows.filterParams.z)));
    float visible=0,samples=float((2*extent+1)*(2*extent+1));
    for(int y=-extent;y<=extent;++y)for(int x=-extent;x<=extent;++x)
        visible+=receiver<=image.sample(imageSampler,clamp(coordinate+float2(x,y)*texel,minimum,maximum),level(0))?1:0;
    return 1-clamp(strength,0.0f,1.0f)*(1-visible/max(samples,1.0f));
}
float TransparentVisibility(int type,int lightIndex,float3 position,float3 normal,float3 lightDir,uint flags,
    constant TransparentLights& lights,constant TransparentShadows& shadows,depth2d<float> image,sampler imageSampler)
{
    if((flags&32u)==0u || dot(normal,lightDir)<=0)return 1;
    float4 views=type==1?shadows.directionalViews[lightIndex]:(type==2?shadows.pointViews[lightIndex]:shadows.spotViews[lightIndex]);
    int count=int(views.y+.5f);if(count==0)return 1;
    int local=0;float depth=0;
    if(type==1)
    {
        depth=-(shadows.cameraView*float4(position,1)).z;
        float lastSplit=shadows.directionalSplits[lightIndex][count-1];
        if(lastSplit>0 && depth>lastSplit)return 1;
        while(local<count-1 && depth>shadows.directionalSplits[lightIndex][local])++local;
    }
    if(type==2)
    {
        float3 direction=-lightDir,magnitude=abs(direction);
        if(magnitude.x>=magnitude.y && magnitude.x>=magnitude.z)local=direction.x>=0?0:1;
        else if(magnitude.y>=magnitude.z)local=direction.y>=0?2:3;
        else local=direction.z>=0?4:5;
    }
    int index=int(views.x+.5f)+local;
    float visibility=TransparentShadowView(index,position,normal,lightDir,views.z,views.w,type==2?int(views.x+.5):-1,lights,shadows,image,imageSampler);
    if(type==1 && local<count-1)
    {
        float previous=local==0?shadows.filterParams.w:shadows.directionalSplits[lightIndex][local-1];
        float split=shadows.directionalSplits[lightIndex][local],blend=smoothstep(mix(previous,split,.9f),split,depth);
        if(blend>0)visibility=mix(visibility,TransparentShadowView(index+1,position,normal,lightDir,views.z,views.w,type==2?int(views.x+.5):-1,lights,shadows,image,imageSampler),blend);
    }
    return visibility;
}
float3 TransparentFresnel(float cosine,float3 f0) {return f0+(1-f0)*pow(clamp(1-cosine,0.0f,1.0f),5.0f);}
float2 TransparentEnvUV(float3 d,constant TransparentFrame& frame)
{
    float c=cos(frame.settings.z),s=sin(frame.settings.z);d.xy=float2(c*d.x+s*d.y,-s*d.x+c*d.y);
    return float2(atan2(d.y,d.x)*(.5f/TransparentPI)+.5f,acos(clamp(d.z,-1.0f,1.0f))/TransparentPI);
}
float3 TransparentEnvMip(texture2d<float> image,float2 coordinate,int mip)
{
    const int2 size=int2(image.get_width(uint(mip)),image.get_height(uint(mip)));
    float2 pixel=coordinate*float2(size)-.5f,w=fract(pixel);int2 a=int2(floor(pixel)),b=a+1;
    const int2 wrappedX=DyWrapBilinearX(a.x,b.x,size.x);
    a.x=wrappedX.x;b.x=wrappedX.y;
    a.y=clamp(a.y,0,size.y-1);b.y=clamp(b.y,0,size.y-1);
    return mix(mix(image.read(uint2(a),uint(mip)),image.read(uint2(b.x,a.y),uint(mip)),w.x),
        mix(image.read(uint2(a.x,b.y),uint(mip)),image.read(uint2(b),uint(mip)),w.x),w.y).rgb;
}
float3 TransparentBoxDirection(float3 position,float3 direction,float3 center,float3 minimum,float3 maximum)
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
float3 TransparentProbeDirection(float3 position,float3 direction,constant TransparentFrame& frame)
{
    return frame.probePosition.w>.5f?TransparentBoxDirection(position,direction,frame.probePosition.xyz,
        frame.probeBoxMin.xyz,frame.probeBoxMax.xyz):direction;
}
float3 TransparentSpatialMip(texture2d_array<float> image,float2 coordinate,int mip,uint layer)
{
    const int2 dimensions=int2(image.get_width(uint(mip)),image.get_height(uint(mip)));
    float2 pixel=coordinate*float2(dimensions)-.5f,w=fract(pixel);int2 a=int2(floor(pixel)),b=a+1;
    const int2 wrappedX=DyWrapBilinearX(a.x,b.x,dimensions.x);
    a.x=wrappedX.x;b.x=wrappedX.y;
    a.y=clamp(a.y,0,dimensions.y-1);b.y=clamp(b.y,0,dimensions.y-1);
    return mix(mix(image.read(uint2(a),layer,uint(mip)),image.read(uint2(b.x,a.y),layer,uint(mip)),w.x),
        mix(image.read(uint2(a.x,b.y),layer,uint(mip)),image.read(uint2(b),layer,uint(mip)),w.x),w.y).rgb;
}
inline bool NativeEnvironmentSample(float2 coordinate,float mip,float maximumMip,uint enabled,uint nativeAllowed)
{
    return nativeAllowed==1u && enabled==1u && all(coordinate>=0) && all(coordinate<=1)
        && mip>=0 && mip<=maximumMip && maximumMip<=32;
}
float TransparentProbeVisibility(float3 position,const device TransparentSpatialProbe& probe,uint index,
    texture2d_array<float> image,uint count)
{
    return SpatialProbeVisibility(position,probe.positionIntensity.xyz,probe.sh[0].w,probe.sh[1].w,count+index,image);
}
float3 TransparentRadiance(texture2d<float> image,float3 position,float3 d,float roughness,constant TransparentFrame& frame,
    texture2d_array<float> probeImage,const device TransparentSpatialProbe* probes,constant uint4& probeParameters,sampler environmentLinearSampler,uint nativeAllowed)
{
    float mip=roughness*frame.settings.w;int lo=int(floor(mip)),hi=min(lo+1,int(frame.settings.w));
    float2 coordinate=TransparentEnvUV(TransparentProbeDirection(position,d,frame),frame);
    float3 sum=0;float weights=0,coverage=0;
    for(uint i=0;i<probeParameters.x;++i)
    {
        const device TransparentSpatialProbe& probe=probes[i];float edge;
        float weight=TransparentProbeWeight(position,probe,edge);if(weight<=0)continue;
        float3 direction=TransparentBoxDirection(position,d,probe.positionIntensity.xyz,probe.boxMinFade.xyz,probe.boxMaxMip.xyz);
        float2 localUV=float2(atan2(direction.y,direction.x)*.15915494309189535f+.5f,
            acos(clamp(direction.z,-1.0f,1.0f))*.3183098861837907f);
        float localMip=roughness*probe.boxMaxMip.w;int a=int(floor(localMip)),b=min(a+1,int(probe.boxMaxMip.w));
        float3 sampledRadiance;
        if(NativeEnvironmentSample(localUV,localMip,probe.boxMaxMip.w,probeParameters.z,nativeAllowed))
            sampledRadiance=probeImage.sample(environmentLinearSampler,localUV,i,level(localMip)).rgb;
        else sampledRadiance=mix(TransparentSpatialMip(probeImage,localUV,a,i),TransparentSpatialMip(probeImage,localUV,b,i),fract(localMip));
        sum+=sampledRadiance*probe.positionIntensity.w*
            (weight*TransparentProbeVisibility(position,probe,i,probeImage,probeParameters.x));
        weights+=weight;coverage+=edge;
    }
    if(probeParameters.y!=0u && weights>0 && coverage>=1 && all(isfinite(coordinate)) && isfinite(mip))
    {
        float3 local=sum/weights;
        if(all(abs(local)<=2.1267646e37f))return local;
    }
    float3 fallback;
    if(NativeEnvironmentSample(coordinate,mip,frame.settings.w,probeParameters.z,nativeAllowed))
        fallback=image.sample(environmentLinearSampler,coordinate,level(mip)).rgb;
    else fallback=mix(TransparentEnvMip(image,coordinate,lo),TransparentEnvMip(image,coordinate,hi),fract(mip));
    fallback*=frame.settings.y;
    return weights>0?mix(fallback,sum/weights,clamp(coverage,0.0f,1.0f)):fallback;
}
float3 TransparentIrradiance(float3 position,float3 n,constant TransparentFrame& frame,
    const device TransparentSpatialProbe* probes,constant uint4& probeParameters,texture2d_array<float> probeImage)
{
    float3 worldNormal=n;
    float c=cos(frame.settings.z),s=sin(frame.settings.z);n.xy=float2(c*n.x+s*n.y,-s*n.x+c*n.y);
    float3 irradiance=(frame.sh[0].rgb*.28209479f+frame.sh[1].rgb*(.48860251f*n.y)+frame.sh[2].rgb*(.48860251f*n.z)+
        frame.sh[3].rgb*(.48860251f*n.x)+frame.sh[4].rgb*(1.09254843f*n.x*n.y)+frame.sh[5].rgb*(1.09254843f*n.y*n.z)+
        frame.sh[6].rgb*(.31539156f*(3*n.z*n.z-1))+frame.sh[7].rgb*(1.09254843f*n.x*n.z)+
        frame.sh[8].rgb*(.54627422f*(n.x*n.x-n.y*n.y)))*frame.settings.y;
    float3 sum=0;float weights=0,coverage=0;
    for(uint i=0;i<probeParameters.x;++i)
    {
        float edge;float weight=TransparentProbeWeight(position,probes[i],edge);if(weight<=0)continue;
        sum+=TransparentProbeIrradiance(probes[i],worldNormal)*
            (weight*TransparentProbeVisibility(position,probes[i],i,probeImage,probeParameters.x));
        weights+=weight;coverage+=edge;
    }
    if(weights>0)irradiance=mix(irradiance,sum/weights,clamp(coverage,0.0f,1.0f));
    return max(irradiance,float3(0))/TransparentPI;
}
float3 TransparentDirect(float3 n,float3 v,float3 l,float3 energy,float3 albedo,float3 f0,float roughness,float diffuse)
{
    float nl=max(dot(n,l),0.0f),nv=max(dot(n,v),.001f);if(nl<=0 || dot(v+l,v+l)<1e-8f)return 0;
    float3 h=normalize(v+l);float nh=clamp(dot(n,h),0.0f,1.0f),a=roughness*roughness,a2=a*a;
    float3 crossNH=cross(n,h);float denominator=dot(crossNH,crossNH)+nh*nh*a2;
    float d=a2/(TransparentPI*denominator*denominator);float k=pow(roughness+1,2.0f)*.125f;
    float g=nv/(nv*(1-k)+k)*nl/(nl*(1-k)+k);float3 f=TransparentFresnel(max(dot(h,v),0.0f),f0);
    return ((1-f)*albedo*(diffuse/TransparentPI)+d*g*f/max(4*nv*nl,.0001f))*energy*nl;
}
float TransparentFalloff(float distance,float range)
{
    if(range<=0 || distance>=range)return 0;float x=distance/range;
    return pow(max(1-x*x*x*x,0.0f),2.0f)/max(distance*distance,.0001f);
}
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
    float4 inverseMatrix,thread const float3* points,int count)
{
    float3 tangent=viewDir-normal*dot(normal,viewDir);
    if(dot(tangent,tangent)<1e-8)
        tangent=cross(abs(normal.z)<.999?float3(0,0,1):float3(0,1,0),normal);
    tangent=normalize(tangent);
    float3 bitangent=cross(normal,tangent);
    float3 previous=LtcTransform(points[count-1]-position,tangent,bitangent,normal,inverseMatrix);
    float3 first=float3(0),last=float3(0);bool haveVertex=false;float sum=0;
    for(int edge=0;edge<count;++edge)
    {
        float3 current=LtcTransform(points[edge]-position,tangent,bitangent,normal,inverseMatrix);
        bool previousInside=previous.z>=0,currentInside=current.z>=0;
        // z=0 반구로 다각형을 자르되 정점 배열 대신 경계 순서대로 적분한다.
        for(int candidate=0;candidate<2;++candidate)
        {
            float3 point;
            if(candidate==0)
            {
                if(previousInside==currentInside)continue;
                point=mix(previous,current,clamp(previous.z/(previous.z-current.z),0.0,1.0));
                point.z=0;
            }
            else
            {
                if(!currentInside)continue;
                point=current;
            }
            float squared=dot(point,point);
            if(squared<=1e-20)continue;
            point*=rsqrt(squared);
            if(haveVertex)sum+=LtcEdgeIntegral(last,point);else {first=point;haveVertex=true;}
            last=point;
        }
        previous=current;
    }
    if(haveVertex)sum+=LtcEdgeIntegral(last,first);
    return clamp(abs(sum),0.0,1.0);
}
inline float3 EvaluateLtcPolygon(float3 normal,float3 viewDir,float3 position,thread const float3* points,int count,
    float3 albedo,float3 f0,float diffuseFactor,float roughness,float3 radiance,texture2d<float> ltcMatrixTexture,texture2d<float> ltcAmplitudeTexture,sampler materialSampler)
{
    float ndotv=clamp(dot(normal,viewDir),0.0,1.0);
    if(ndotv<=0)return float3(0);
    float2 coordinate=(float2(clamp(roughness,0.0,1.0),sqrt(1-ndotv))*63.0+.5)/64.0;
    float4 inverseMatrix=ltcMatrixTexture.sample(materialSampler,coordinate,level(0));
    float4 amplitude=ltcAmplitudeTexture.sample(materialSampler,coordinate,level(0));
    float specular=LtcPolygonIntegral(normal,viewDir,position,inverseMatrix,points,count);
    float diffuse=LtcPolygonIntegral(normal,viewDir,position,float4(1,0,0,1),points,count);
    float3 specularWeight=f0*max(amplitude.x,0.0)+(float3(1)-f0)*max(amplitude.y,0.0);
    float3 diffuseWeight=(float3(1)-TransparentFresnel(ndotv,f0))*diffuseFactor;
    // 정규화된 cosine 적분이므로 diffuse에 1/pi나 거리 감쇠를 다시 곱하지 않는다.
    return radiance*(specularWeight*specular+diffuseWeight*albedo*diffuse);
}
inline float3 EvaluateRectAreaLight(RendererRectAreaLight light,float3 normal,float3 viewDir,
    float3 albedo,float3 f0,float diffuseFactor,float roughness,float3 worldPosition,texture2d<float> ltcMatrixTexture,texture2d<float> ltcAmplitudeTexture,sampler materialSampler)
{
    float3 direction=normalize(light.directionWidth.xyz);
    if(light.directionWidth.w<=0 || light.upHeight.w<=0 || dot(direction,worldPosition-light.positionIntensity.xyz)<=0)
        return float3(0);
    float3 right=normalize(cross(direction,light.upHeight.xyz));
    float3 up=normalize(cross(right,direction));
    float3 x=right*(light.directionWidth.w*.5),y=up*(light.upHeight.w*.5),center=light.positionIntensity.xyz;
    float3 points[16];
    points[0]=center-x-y;points[1]=center+x-y;points[2]=center+x+y;points[3]=center-x+y;
    return EvaluateLtcPolygon(normal,viewDir,worldPosition,points,4,albedo,f0,diffuseFactor,roughness,
        light.color.rgb*max(light.positionIntensity.w,0.0),ltcMatrixTexture,ltcAmplitudeTexture,materialSampler);
}
inline float3 EvaluateDiscAreaLight(RendererDiscAreaLight light,float3 normal,float3 viewDir,
    float3 albedo,float3 f0,float diffuseFactor,float roughness,float3 worldPosition,texture2d<float> ltcMatrixTexture,texture2d<float> ltcAmplitudeTexture,sampler materialSampler)
{
    float3 direction=normalize(light.directionRadius.xyz);
    if(light.directionRadius.w<=0 || dot(direction,worldPosition-light.positionIntensity.xyz)<=0)return float3(0);
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
        light.color.rgb*max(light.positionIntensity.w,0.0),ltcMatrixTexture,ltcAmplitudeTexture,materialSampler);
}
#endif

[[early_fragment_tests]]
fragment TransparentOutput TRANSPARENT_FRAGMENT_ENTRY(TransparentVertex input [[stage_in]],bool frontFacing [[front_facing]],
    texture2d<float> baseTexture [[texture(0)]],texture2d<float> materialTexture [[texture(1)]],
    texture2d<float> normalTexture [[texture(2)]],texture2d<float> aoTexture [[texture(3)]],
    texture2d<float> emissiveTexture [[texture(4)]],texture2d<float> opaqueTexture [[texture(5)]],
    texture2d<float> depthTexture [[texture(6)]],texture2d<float> prefilterTexture [[texture(7)]],
    depth2d<float> shadowTexture [[texture(14)]],
    sampler materialSampler [[sampler(8)]],sampler screenSampler [[sampler(9)]],sampler pointSampler [[sampler(10)]],
    sampler environmentLinearSampler [[sampler(15)]],
    device const TransparentDraw* draws [[buffer(11)]],constant TransparentFrame& frame [[buffer(12)]],
    constant TransparentLights& lights [[buffer(13)]],constant uint4& index [[buffer(15)]],
    constant TransparentShadows& shadows [[buffer(16)]],
    const device ClusterLight* clusterLights [[buffer(19)]],
    const device uint* clusterList [[buffer(20)]],
    constant ClusterParameters& clusters [[buffer(21)]],
    const device TransparentSpatialProbe* probes [[buffer(22)]],
    texture2d_array<float> probePrefilterTexture [[texture(23)]],
    constant uint4& probeParameters [[buffer(24)]],
    texture2d<float> ltcMatrixTexture [[texture(26)]],
    texture2d<float> ltcAmplitudeTexture [[texture(27)]],texture2d<float> nearestDepthTexture [[texture(28)]])
{
    // Preserve material gradients while all lanes still participate.
    float2 uvDx=dfdx(input.uv),uvDy=dfdy(input.uv);
    float2 screenUV=input.position.xy*frame.screen.xy;
    float opaqueDepth=depthTexture.read(uint2(input.position.xy)).r;
    bool excludedLayer=false;
    if(index.y!=0u)
    {
        float nearestDepth=nearestDepthTexture.read(uint2(input.position.xy)).r;
        uint currentBits=as_type<uint>(input.position.z),nearestBits=as_type<uint>(nearestDepth);
        // 같은 정점 셰이더의 D32 결과를 한 ULP까지만 비교하고 거리 비례 오차는 넣지 않는다.
        bool frontLayer=max(currentBits,nearestBits)-min(currentBits,nearestBits)<=1u;
        excludedLayer=index.y==1u?frontLayer:!frontLayer;
    }
    TransparentDraw draw=draws[index.x];uint flags=draw.flags.x;
    float3 geometricNormal=float3(0,0,1),n=geometricNormal,v=geometricNormal;
    float normalVariance=0;
    // These flags are uniform for the draw; debug/unlit materials need no normal footprint.
    if((flags&(64u|128u))==0u)
    {
        geometricNormal=TransparentPixelNormalizeOr(input.worldNormal,float3(0,0,1));n=geometricNormal;
        v=TransparentPixelNormalizeOr(frame.camera.xyz-input.worldPosition,geometricNormal);
        if((flags&4u)!=0u)
        {
            float3 fallback=normalize(cross(abs(n.z)<.99f?float3(0,0,1):float3(0,1,0),n));
            float3 t=TransparentPixelNormalizeOr(input.worldTangent.xyz-n*dot(n,input.worldTangent.xyz),fallback),b=cross(n,t)*input.worldTangent.w;
            float3 mapped=normalTexture.sample(materialSampler,input.uv).xyz*2-1;
            mapped.xy*=draw.material.z;mapped.y*=(draw.emissive.w<0?-1.0f:1.0f);
            n=TransparentPixelNormalizeOr(t*mapped.x+b*mapped.y+n*mapped.z,n);
        }
        // Keep mapped-normal derivatives before discard or visible-side selection.
        float3 normalDx=dfdx(n),normalDy=dfdy(n);
        normalVariance=0.25f*(dot(normalDx,normalDx)+dot(normalDy,normalDy));
    }
    if(input.position.z>opaqueDepth || excludedLayer)discard_fragment();
    float4 base=draw.baseColor;
    if((flags&1u)!=0u)base*=baseTexture.sample(materialSampler,input.uv,gradient2d(uvDx,uvDy));
    float2 mr=draw.material.xy;if((flags&2u)!=0u)mr*=materialTexture.sample(materialSampler,input.uv,gradient2d(uvDx,uvDy)).bg;
    float metal=clamp(mr.x,0.0f,1.0f),rough=clamp(mr.y,.04f,1.0f),transmission=clamp(draw.optics.x,0.0f,1.0f)*(1-metal);
    if((flags&64u)!=0u)
    {
        float coverage=mix(clamp(base.a,0.0f,1.0f),1.0f,transmission);
        TransparentOutput output;output.color=float4(draw.baseColor.rgb*coverage,coverage);output.reactive=float4(coverage);return output;
    }
    if((flags&128u)!=0u)
    {
        float coverage=clamp(base.a,0.0f,1.0f);
        TransparentOutput output;output.color=float4(clamp(base.rgb,0.0f,65504.0f)*coverage,coverage);output.reactive=float4(coverage);return output;
    }
    float filteredAlphaSquared=min(1.0f,rough*rough*rough*rough+min(2.0f*normalVariance,0.18f));
    float specularRoughness=sqrt(sqrt(filteredAlphaSquared));
    if(transmission>0)
    {
        // Explicit-thickness thin-wall approximation: either visible mesh side
        // is an air-to-material interface, not an independently traced exit.
        if(dot(geometricNormal,v)<0) {geometricNormal=-geometricNormal;n=-n;}
        if(dot(n,v)<=0)n=geometricNormal;
    }
    else if(draw.flags.y!=0u && !frontFacing)n=-n;
    float ior=draw.optics.y,dielectric=pow((ior-1)/(ior+1),2.0f),nv=max(dot(n,v),0.0f);
    float3 transmissionRay=transmission>0?refract(-v,n,1/ior):float3(0);
    bool totalInternalReflection=transmission>0 && dot(transmissionRay,transmissionRay)<1e-6f;
    float3 f0=totalInternalReflection?float3(1):mix(float3(dielectric),base.rgb,metal),f=TransparentFresnel(nv,f0);
    float ao=1;if((flags&8u)!=0u)ao=mix(1.0f,aoTexture.sample(materialSampler,input.uv).r,draw.material.w);
    float3 lit=(1-f)*base.rgb*(1-metal)*(1-transmission)*TransparentIrradiance(input.worldPosition,n,frame,probes,probeParameters,probePrefilterTexture)*ao;
    if((uint(frame.screen.w+.5f)&2u)!=0u)lit+=TransparentRadiance(prefilterTexture,input.worldPosition,reflect(-v,n),specularRoughness,frame,
        probePrefilterTexture,probes,probeParameters,environmentLinearSampler,index.z)*f*frame.specularColor.rgb;
    for(int i=0;i<clamp(int(lights.counts.x+.5f),0,4);++i)
    {
        float3 l=normalize(lights.directional[i].directionIntensity.xyz);
        float visibility=TransparentVisibility(1,i,input.worldPosition,n,l,flags,lights,shadows,shadowTexture,pointSampler);
        lit+=TransparentDirect(n,v,l,lights.directional[i].color.rgb*lights.directional[i].directionIntensity.w*visibility,
            base.rgb,f0,specularRoughness,(1-metal)*(1-transmission));
    }
    uint clusterBase=ClusterBase(input.worldPosition,clusters),listed=0xffffffffu;
    if(clusterBase!=0xffffffffu)listed=clusterList[clusterBase];
    uint localCount=listed==0xffffffffu?clusters.counts.x:listed;
    for(uint item=0u;item<localCount;++item)
    {
        uint lightIndex=listed==0xffffffffu?item:clusterList[clusterBase+1u+item];
        ClusterLight light=clusterLights[lightIndex];
        float3 delta=light.positionRange.xyz-input.worldPosition;float distance=length(delta);
        if(distance<=1e-4f || distance>=light.positionRange.w)continue;
        float3 l=delta/distance;
        int type=int(light.coneShadow.y+.5f),shadowIndex=int(light.coneShadow.z);
        float cone=1;
        if(type==3)cone=smoothstep(light.directionOuterCos.w,max(light.coneShadow.x,light.directionOuterCos.w+1e-4f),
            dot(-l,light.directionOuterCos.xyz));
        float visibility=shadowIndex>=0?TransparentVisibility(type,shadowIndex,input.worldPosition,n,l,flags,lights,shadows,shadowTexture,pointSampler):1;
        lit+=TransparentDirect(n,v,l,light.colorIntensity.rgb*light.colorIntensity.w*TransparentFalloff(distance,light.positionRange.w)*cone*visibility,
            base.rgb,f0,specularRoughness,(1-metal)*(1-transmission));
    }
#if RENDERER_ENABLE_AREA_LIGHTS
    for(int i=0;i<clamp(int(lights.areaCounts.x+.5f),0,4);++i)
        lit+=EvaluateRectAreaLight(lights.rectAreaLights[i],n,v,base.rgb,f0,(1-metal)*(1-transmission),specularRoughness,input.worldPosition,
            ltcMatrixTexture,ltcAmplitudeTexture,materialSampler);
    for(int i=0;i<clamp(int(lights.areaCounts.y+.5f),0,4);++i)
        lit+=EvaluateDiscAreaLight(lights.discAreaLights[i],n,v,base.rgb,f0,(1-metal)*(1-transmission),specularRoughness,input.worldPosition,
            ltcMatrixTexture,ltcAmplitudeTexture,materialSampler);
#endif
    float3 refracted=0;
    if(transmission>0 && !totalInternalReflection)
    {
        float3 ray=transmissionRay;
        float path=draw.optics.z/max(abs(dot(n,ray)),.1f);float4 exitClip=frame.viewProjection*float4(input.worldPosition+ray*path,1);
        float2 target=exitClip.xy/max(exitClip.w,1e-5f)*float2(.5f,-.5f)+.5f;
        float exitDepth=exitClip.z/max(exitClip.w,1e-5f);
        bool onScreen=exitClip.w>0 && exitDepth>=0 && exitDepth<=1 && all(target>=0) && all(target<=1);
        if(onScreen && depthTexture.sample(pointSampler,target,level(0)).r<exitDepth)target=screenUV;
        refracted=onScreen?opaqueTexture.sample(screenSampler,target,level(0)).rgb:TransparentRadiance(prefilterTexture,input.worldPosition,ray,rough,frame,
            probePrefilterTexture,probes,probeParameters,environmentLinearSampler,index.z);
        float3 absorption=draw.optics.w>0?pow(clamp(draw.attenuation.rgb,.0001f,1.0f),float3(path/draw.optics.w)):float3(1);
        refracted*=absorption*base.rgb*(1-f)*transmission;
    }
    float3 emissive=draw.emissive.rgb;if((flags&16u)!=0u)emissive*=emissiveTexture.sample(materialSampler,input.uv).rgb;
    float coverage=mix(clamp(base.a,0.0f,1.0f),1.0f,transmission);
    float3 color=lights.counts.w>.5f?lit+refracted+emissive:base.rgb+emissive;
    TransparentOutput output;output.color=float4(clamp(color,0.0f,65504.0f)*coverage,coverage);output.reactive=float4(coverage);return output;
}
