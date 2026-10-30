#version 450
#extension GL_GOOGLE_include_directive : require
#include "probe_visibility.glsl"
layout(location=0) in vec2 uv;
layout(location=1) in vec3 worldPosition;
layout(location=2) in vec3 worldNormal;
layout(location=3) in vec4 worldTangent;
layout(location=0) out vec4 outputColor;
layout(location=1) out vec4 reactive;
struct Draw {mat4 model;mat4 normalMatrix;vec4 baseColor;vec4 emissive;vec4 material;vec4 optics;vec4 attenuation;uvec4 flags;};
layout(std430,set=0,binding=11) readonly buffer Draws {Draw draws[];};
layout(set=0,binding=12) uniform Frame {mat4 viewProjection;mat4 view;mat4 inverseViewProjection;vec4 camera;vec4 sh[9];vec4 settings;vec4 screen;vec4 probePosition;vec4 probeBoxMin;vec4 probeBoxMax;vec4 specularColor;} frame;
layout(push_constant) uniform Index {uvec4 index;};
layout(set=0,binding=0) uniform texture2D baseTexture;
layout(set=0,binding=1) uniform texture2D materialTexture;
layout(set=0,binding=2) uniform texture2D normalTexture;
layout(set=0,binding=3) uniform texture2D aoTexture;
layout(set=0,binding=4) uniform texture2D emissiveTexture;
layout(set=0,binding=5) uniform texture2D opaqueTexture;
layout(set=0,binding=6) uniform texture2D depthTexture;
layout(set=0,binding=7) uniform texture2D prefilterTexture;
layout(set=0,binding=14) uniform texture2D shadowTexture;
layout(set=0,binding=26) uniform texture2D ltcMatrixTexture;
layout(set=0,binding=27) uniform texture2D ltcAmplitudeTexture;
layout(set=0,binding=28) uniform texture2D nearestDepthTexture;
layout(set=0,binding=8) uniform sampler materialSampler;
layout(set=0,binding=9) uniform sampler screenSampler;
layout(set=0,binding=10) uniform sampler pointSampler;
struct Directional {vec4 directionIntensity;vec4 color;};
struct Point {vec4 positionRange;vec4 colorIntensity;};
struct Spot {vec4 positionRange;vec4 directionOuterCos;vec4 colorIntensity;vec4 cone;};
struct RendererRectAreaLight {vec4 positionIntensity;vec4 directionWidth;vec4 upHeight;vec4 color;};
struct RendererDiscAreaLight {vec4 positionIntensity;vec4 directionRadius;vec4 up;vec4 color;};
layout(set=0,binding=13) uniform Lights {vec4 cameraPosition;vec4 direction;vec4 color;vec4 ambient;vec4 shadow;vec4 pbr;
    vec4 environment;vec4 pointPosition;vec4 pointColor;vec4 counts;vec4 areaCounts;
    Directional directional[4];Point point[16];Spot spot[16];
    RendererRectAreaLight rectAreaLights[4];RendererDiscAreaLight discAreaLights[4];vec4 shadowLight;} lights;
layout(set=0,binding=16) uniform ShadowMatrix {
    mat4 lightViewProjectionMatrix[128];vec4 atlasRect[128];
    vec4 directionalViews[4];vec4 pointViews[16];vec4 spotViews[16];
    vec4 directionalSplits[4];mat4 cameraView;vec4 filterParams;
    vec4 viewProjectionParams[128];vec4 softShadowParams;
} shadowMatrix;
const float PI=3.14159265359;
vec3 TransparentPixelNormalizeOr(vec3 value,vec3 fallback)
{
    float scale=max(max(abs(value.x),abs(value.y)),abs(value.z));
    if(!(scale>0) || any(isnan(value)) || any(isinf(value)))return fallback;
    return normalize(value/scale);
}
struct ClusterLight {vec4 positionRange;vec4 colorIntensity;vec4 directionOuterCos;vec4 coneShadow;};
layout(std430,set=0,binding=19) readonly buffer ClusterLights {ClusterLight clusterLights[];};
layout(std430,set=0,binding=20) readonly buffer ClusterLists {uint clusterList[];};
layout(set=0,binding=21) uniform ClusterParameters {mat4 view;mat4 projection;mat4 inverseProjection;vec4 depth;uvec4 dimensions;uvec4 counts;} clusters;
struct SpatialProbe {vec4 positionIntensity;vec4 boxMinFade;vec4 boxMaxMip;vec4 sh[9];};
layout(std430,set=0,binding=22) readonly buffer ProbeData {SpatialProbe probes[];};
layout(set=0,binding=23) uniform texture2DArray probePrefilterTexture;
layout(set=0,binding=24) uniform ProbeParameters {uvec4 probeParameters;};
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
uint ClusterBase(vec3 position)
{
    if(clusters.counts.y==0u)return 0xffffffffu;
    vec4 viewPosition=clusters.view*vec4(position,1),clip=clusters.projection*viewPosition;
    float distance=-viewPosition.z;
    if(distance<clusters.depth.x || distance>clusters.depth.y || clip.w<=0)return 0xffffffffu;
    vec2 coordinate=clip.xy/clip.w*.5+.5;
    if(any(lessThan(coordinate,vec2(0))) || any(greaterThanEqual(coordinate,vec2(1))))return 0xffffffffu;
    float slice=clusters.counts.z!=0u?log(distance/clusters.depth.x)/clusters.depth.z:
        (distance-clusters.depth.x)/(clusters.depth.y-clusters.depth.x);
    uvec3 index=min(uvec3(coordinate*vec2(clusters.dimensions.xy),slice*float(clusters.dimensions.z)),clusters.dimensions.xyz-uvec3(1));
    return (index.x+clusters.dimensions.x*(index.y+clusters.dimensions.y*index.z))*(clusters.dimensions.w+1u);
}
#define DY_SHADOW_PROJECTION(index) shadowMatrix.viewProjectionParams[index]
#define DY_SHADOW_MATRIX(index) shadowMatrix.lightViewProjectionMatrix[index]
#define DY_SHADOW_TILE(index) shadowMatrix.atlasRect[index]
#define DY_SHADOW_DIMENSIONS textureSize(sampler2D(shadowTexture,pointSampler),0)
#define DY_SHADOW_READ(pixel) texelFetch(sampler2D(shadowTexture,pointSampler),pixel,0).r
#define DY_SHADOW_FILTER shadowMatrix.filterParams
#define DY_SHADOW_SOFT shadowMatrix.softShadowParams
#include "shadow_filter.glsl"

float ShadowView(int index,vec3 position,vec3 normal,vec3 lightDir,float strength,float sourceRadius,int pointBase)
{
    vec4 clip=shadowMatrix.lightViewProjectionMatrix[index]*vec4(position,1);
    if(clip.w<=0)return 1;
    vec3 ndc=clip.xyz/clip.w;vec2 coordinate=ndc.xy*.5+.5;coordinate.y=1-coordinate.y;
    if(any(lessThan(coordinate,vec2(0))) || any(greaterThan(coordinate,vec2(1))) || ndc.z<0 || ndc.z>1)return 1;
    vec4 tile=shadowMatrix.atlasRect[index];vec2 texel=1.0/vec2(textureSize(sampler2D(shadowTexture,pointSampler),0));
    vec2 minimum=tile.xy+texel*.5,maximum=tile.xy+tile.zw-texel*.5;
    vec2 localCoordinate=coordinate;
    coordinate=tile.xy+coordinate*tile.zw;
    float angle=1-max(dot(normal,lightDir),0);
    float receiver=ndc.z-max(lights.shadow.x,lights.shadow.y*angle)-lights.shadow.z*angle;
    if(sourceRadius>0 && shadowMatrix.softShadowParams.x>0)
        return 1-clamp(strength,0.0,1.0)*(1-DyShadowPcss(index,pointBase,localCoordinate,receiver,clip.w,lightDir,sourceRadius));
    int extent=int(ceil(min(lights.shadow.w,shadowMatrix.filterParams.z)));
    float visible=0,samples=float((2*extent+1)*(2*extent+1));
    for(int y=-extent;y<=extent;++y)for(int x=-extent;x<=extent;++x)
        visible+=receiver<=textureLod(sampler2D(shadowTexture,pointSampler),clamp(coordinate+vec2(x,y)*texel,minimum,maximum),0).r?1:0;
    return 1-clamp(strength,0,1)*(1-visible/max(samples,1));
}
float Visibility(int type,int lightIndex,vec3 position,vec3 normal,vec3 lightDir,uint flags)
{
    if((flags&32u)==0u || dot(normal,lightDir)<=0)return 1;
    vec4 views=type==1?shadowMatrix.directionalViews[lightIndex]:(type==2?shadowMatrix.pointViews[lightIndex]:shadowMatrix.spotViews[lightIndex]);
    int count=int(views.y+.5);if(count==0)return 1;
    int local=0;float depth=0;
    if(type==1)
    {
        depth=-(shadowMatrix.cameraView*vec4(position,1)).z;
        float lastSplit=shadowMatrix.directionalSplits[lightIndex][count-1];
        if(lastSplit>0 && depth>lastSplit)return 1;
        while(local<count-1 && depth>shadowMatrix.directionalSplits[lightIndex][local])++local;
    }
    if(type==2)
    {
        vec3 direction=-lightDir,magnitude=abs(direction);
        if(magnitude.x>=magnitude.y && magnitude.x>=magnitude.z)local=direction.x>=0?0:1;
        else if(magnitude.y>=magnitude.z)local=direction.y>=0?2:3;
        else local=direction.z>=0?4:5;
    }
    int index=int(views.x+.5)+local;float visibility=ShadowView(index,position,normal,lightDir,views.z,views.w,type==2?int(views.x+.5):-1);
    if(type==1 && local<count-1)
    {
        float previous=local==0?shadowMatrix.filterParams.w:shadowMatrix.directionalSplits[lightIndex][local-1];
        float split=shadowMatrix.directionalSplits[lightIndex][local],blend=smoothstep(mix(previous,split,.9),split,depth);
        if(blend>0)visibility=mix(visibility,ShadowView(index+1,position,normal,lightDir,views.z,views.w,type==2?int(views.x+.5):-1),blend);
    }
    return visibility;
}
vec3 Fresnel(float cosine,vec3 f0) {return f0+(1-f0)*pow(clamp(1-cosine,0,1),5);}
vec2 EnvUV(vec3 d)
{
    float c=cos(frame.settings.z),s=sin(frame.settings.z);d.xy=mat2(c,-s,s,c)*d.xy;
    return vec2(atan(d.y,d.x)*(.5/PI)+.5,acos(clamp(d.z,-1,1))/PI);
}
vec3 EnvMip(vec2 coordinate,int mip)
{
    ivec2 size=textureSize(sampler2D(prefilterTexture,pointSampler),mip);
    vec2 pixel=coordinate*vec2(size)-.5,w=fract(pixel);ivec2 a=ivec2(floor(pixel)),b=a+1;
    a.x=(a.x%size.x+size.x)%size.x;b.x=(b.x%size.x+size.x)%size.x;
    a.y=clamp(a.y,0,size.y-1);b.y=clamp(b.y,0,size.y-1);
    return mix(mix(texelFetch(sampler2D(prefilterTexture,pointSampler),a,mip),texelFetch(sampler2D(prefilterTexture,pointSampler),ivec2(b.x,a.y),mip),w.x),
        mix(texelFetch(sampler2D(prefilterTexture,pointSampler),ivec2(a.x,b.y),mip),texelFetch(sampler2D(prefilterTexture,pointSampler),b,mip),w.x),w.y).rgb;
}
vec3 BoxDirection(vec3 position,vec3 direction,vec3 center,vec3 minimum,vec3 maximum)
{
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
    ivec2 dimensions=textureSize(sampler2DArray(probePrefilterTexture,pointSampler),mip).xy;
    vec2 pixel=coordinate*vec2(dimensions)-.5,w=fract(pixel);ivec2 a=ivec2(floor(pixel)),b=a+1;
    a.x=(a.x%dimensions.x+dimensions.x)%dimensions.x;b.x=(b.x%dimensions.x+dimensions.x)%dimensions.x;
    a.y=clamp(a.y,0,dimensions.y-1);b.y=clamp(b.y,0,dimensions.y-1);
    return mix(mix(texelFetch(sampler2DArray(probePrefilterTexture,pointSampler),ivec3(a,layer),mip),
        texelFetch(sampler2DArray(probePrefilterTexture,pointSampler),ivec3(b.x,a.y,layer),mip),w.x),
        mix(texelFetch(sampler2DArray(probePrefilterTexture,pointSampler),ivec3(a.x,b.y,layer),mip),
        texelFetch(sampler2DArray(probePrefilterTexture,pointSampler),ivec3(b,layer),mip),w.x),w.y).rgb;
}
float ProbeVisibility(vec3 position,SpatialProbe probe,uint index)
{
    return SpatialProbeVisibility(position,probe.positionIntensity.xyz,probe.sh[0].w,probe.sh[1].w,
        probeParameters.x+index,probePrefilterTexture,pointSampler);
}
vec3 Radiance(vec3 position,vec3 d,float roughness)
{
    float mip=roughness*frame.settings.w;int lo=int(floor(mip)),hi=min(lo+1,int(frame.settings.w));
    vec2 coordinate=EnvUV(ProbeDirection(position,d));
    vec3 fallback=mix(EnvMip(coordinate,lo),EnvMip(coordinate,hi),fract(mip))*frame.settings.y;
    vec3 sum=vec3(0);float weights=0,coverage=0;
    for(uint i=0u;i<probeParameters.x;++i)
    {
        float edge;float weight=ProbeWeight(position,probes[i],edge);if(weight<=0)continue;
        vec3 direction=BoxDirection(position,d,probes[i].positionIntensity.xyz,probes[i].boxMinFade.xyz,probes[i].boxMaxMip.xyz);
        vec2 localUV=vec2(atan(direction.y,direction.x)*.15915494309189535+.5,acos(clamp(direction.z,-1,1))*.3183098861837907);
        float localMip=roughness*probes[i].boxMaxMip.w;int a=int(floor(localMip)),b=min(a+1,int(probes[i].boxMaxMip.w));
        sum+=mix(SpatialMip(localUV,a,int(i)),SpatialMip(localUV,b,int(i)),fract(localMip))*probes[i].positionIntensity.w*
            (weight*ProbeVisibility(position,probes[i],i));
        weights+=weight;coverage+=edge;
    }
    return weights>0?mix(fallback,sum/weights,clamp(coverage,0,1)):fallback;
}
vec3 Irradiance(vec3 position,vec3 n)
{
    vec3 worldNormal=n;
    float c=cos(frame.settings.z),s=sin(frame.settings.z);n.xy=mat2(c,-s,s,c)*n.xy;
    vec3 irradiance=(frame.sh[0].rgb*.28209479+frame.sh[1].rgb*(.48860251*n.y)+frame.sh[2].rgb*(.48860251*n.z)+
        frame.sh[3].rgb*(.48860251*n.x)+frame.sh[4].rgb*(1.09254843*n.x*n.y)+frame.sh[5].rgb*(1.09254843*n.y*n.z)+
        frame.sh[6].rgb*(.31539156*(3*n.z*n.z-1))+frame.sh[7].rgb*(1.09254843*n.x*n.z)+
        frame.sh[8].rgb*(.54627422*(n.x*n.x-n.y*n.y)))*frame.settings.y;
    vec3 sum=vec3(0);float weights=0,coverage=0;
    for(uint i=0u;i<probeParameters.x;++i)
    {
        float edge;float weight=ProbeWeight(position,probes[i],edge);if(weight<=0)continue;
        sum+=ProbeIrradiance(probes[i],worldNormal)*(weight*ProbeVisibility(position,probes[i],i));
        weights+=weight;coverage+=edge;
    }
    if(weights>0)irradiance=mix(irradiance,sum/weights,clamp(coverage,0,1));
    return max(irradiance,vec3(0))/PI;
}
vec3 Direct(vec3 n,vec3 v,vec3 l,vec3 energy,vec3 albedo,vec3 f0,float roughness,float diffuse)
{
    float nl=max(dot(n,l),0),nv=max(dot(n,v),.001);if(nl<=0 || dot(v+l,v+l)<1e-8)return vec3(0);
    vec3 h=normalize(v+l);float nh=clamp(dot(n,h),0,1),a=roughness*roughness,a2=a*a;
    vec3 crossNH=cross(n,h);float denominator=dot(crossNH,crossNH)+nh*nh*a2;
    float d=a2/(PI*denominator*denominator);float k=pow(roughness+1,2)*.125;
    float g=nv/(nv*(1-k)+k)*nl/(nl*(1-k)+k);vec3 f=Fresnel(max(dot(h,v),0),f0);
    return ((1-f)*albedo*(diffuse/PI)+d*g*f/max(4*nv*nl,.0001))*energy*nl;
}
float Falloff(float distance,float range)
{
    if(range<=0 || distance>=range)return 0;float x=distance/range;
    return pow(max(1-x*x*x*x,0),2)/max(distance*distance,.0001);
}
// LTC 적분과 표 배치는 Heitz 외 SIGGRAPH 2016 및 Assets/ltc/LICENSE를 따른다.
float LtcEdgeIntegral(vec3 a,vec3 b)
{
    vec3 edge=cross(a,b);
    float sine=length(edge),cosine=clamp(dot(a,b),-1.0,1.0);
    return edge.z*(atan(sine,cosine)/max(sine,1e-12))*0.15915494309189535;
}
vec3 LtcTransform(vec3 offset,vec3 tangent,vec3 bitangent,vec3 normal,vec4 inverseMatrix)
{
    vec3 local=vec3(dot(offset,tangent),dot(offset,bitangent),dot(offset,normal));
    return vec3(inverseMatrix.x*local.x+inverseMatrix.z*local.z,local.y,
        inverseMatrix.y*local.x+inverseMatrix.w*local.z);
}
float LtcPolygonIntegral(vec3 normal,vec3 viewDir,vec3 position,
    vec4 inverseMatrix,vec3 points[16],int count)
{
    vec3 tangent=viewDir-normal*dot(normal,viewDir);
    if(dot(tangent,tangent)<1e-8)
        tangent=cross(abs(normal.z)<.999?vec3(0,0,1):vec3(0,1,0),normal);
    tangent=normalize(tangent);
    vec3 bitangent=cross(normal,tangent);
    vec3 previous=LtcTransform(points[count-1]-position,tangent,bitangent,normal,inverseMatrix);
    vec3 first=vec3(0),last=vec3(0);bool haveVertex=false;float sum=0;
    for(int edge=0;edge<count;++edge)
    {
        vec3 current=LtcTransform(points[edge]-position,tangent,bitangent,normal,inverseMatrix);
        bool previousInside=previous.z>=0,currentInside=current.z>=0;
        // z=0 반구로 다각형을 자르되 정점 배열 대신 경계 순서대로 적분한다.
        for(int candidate=0;candidate<2;++candidate)
        {
            vec3 point;
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
            point*=inversesqrt(squared);
            if(haveVertex)sum+=LtcEdgeIntegral(last,point);else {first=point;haveVertex=true;}
            last=point;
        }
        previous=current;
    }
    if(haveVertex)sum+=LtcEdgeIntegral(last,first);
    return clamp(abs(sum),0.0,1.0);
}
vec3 EvaluateLtcPolygon(vec3 normal,vec3 viewDir,vec3 position,vec3 points[16],int count,
    vec3 albedo,vec3 f0,float diffuseFactor,float roughness,vec3 radiance)
{
    float ndotv=clamp(dot(normal,viewDir),0.0,1.0);
    if(ndotv<=0)return vec3(0);
    vec2 coordinate=(vec2(clamp(roughness,0.0,1.0),sqrt(1-ndotv))*63.0+.5)/64.0;
    vec4 inverseMatrix=textureLod(sampler2D(ltcMatrixTexture,materialSampler),coordinate,0);
    vec4 amplitude=textureLod(sampler2D(ltcAmplitudeTexture,materialSampler),coordinate,0);
    float specular=LtcPolygonIntegral(normal,viewDir,position,inverseMatrix,points,count);
    float diffuse=LtcPolygonIntegral(normal,viewDir,position,vec4(1,0,0,1),points,count);
    vec3 specularWeight=f0*max(amplitude.x,0.0)+(vec3(1)-f0)*max(amplitude.y,0.0);
    vec3 diffuseWeight=(vec3(1)-Fresnel(ndotv,f0))*diffuseFactor;
    // 정규화된 cosine 적분이므로 diffuse에 1/pi나 거리 감쇠를 다시 곱하지 않는다.
    return radiance*(specularWeight*specular+diffuseWeight*albedo*diffuse);
}
vec3 EvaluateRectAreaLight(RendererRectAreaLight light,vec3 normal,vec3 viewDir,
    vec3 albedo,vec3 f0,float diffuseFactor,float roughness,vec3 worldPosition)
{
    vec3 direction=normalize(light.directionWidth.xyz);
    if(light.directionWidth.w<=0 || light.upHeight.w<=0 || dot(direction,worldPosition-light.positionIntensity.xyz)<=0)
        return vec3(0);
    vec3 right=normalize(cross(direction,light.upHeight.xyz));
    vec3 up=normalize(cross(right,direction));
    vec3 x=right*(light.directionWidth.w*.5),y=up*(light.upHeight.w*.5),center=light.positionIntensity.xyz;
    vec3 points[16];
    points[0]=center-x-y;points[1]=center+x-y;points[2]=center+x+y;points[3]=center-x+y;
    return EvaluateLtcPolygon(normal,viewDir,worldPosition,points,4,albedo,f0,diffuseFactor,roughness,
        light.color.rgb*max(light.positionIntensity.w,0.0));
}
vec3 EvaluateDiscAreaLight(RendererDiscAreaLight light,vec3 normal,vec3 viewDir,
    vec3 albedo,vec3 f0,float diffuseFactor,float roughness,vec3 worldPosition)
{
    vec3 direction=normalize(light.directionRadius.xyz);
    if(light.directionRadius.w<=0 || dot(direction,worldPosition-light.positionIntensity.xyz)<=0)return vec3(0);
    vec3 right=normalize(cross(direction,light.up.xyz));
    vec3 up=normalize(cross(right,direction));
    vec3 points[16];
    // 원판 경계를 16각형으로 근사한 뒤 면 전체를 LTC로 적분한다.
    for(int corner=0;corner<16;++corner)
    {
        float angle=float(corner)*0.39269908169872415;
        points[corner]=light.positionIntensity.xyz+(right*cos(angle)+up*sin(angle))*light.directionRadius.w;
    }
    return EvaluateLtcPolygon(normal,viewDir,worldPosition,points,16,albedo,f0,diffuseFactor,roughness,
        light.color.rgb*max(light.positionIntensity.w,0.0));
}

void main()
{
    vec2 screenUV=gl_FragCoord.xy*frame.screen.xy;
    float opaqueDepth=texelFetch(sampler2D(depthTexture,pointSampler),ivec2(gl_FragCoord.xy),0).r;
    bool excludedLayer=false;
    if(index.y!=0u)
    {
        float nearestDepth=texelFetch(sampler2D(nearestDepthTexture,pointSampler),ivec2(gl_FragCoord.xy),0).r;
        uint currentBits=floatBitsToUint(gl_FragCoord.z),nearestBits=floatBitsToUint(nearestDepth);
        // 같은 정점 셰이더의 D32 결과를 한 ULP까지만 비교하고 거리 비례 오차는 넣지 않는다.
        bool frontLayer=max(currentBits,nearestBits)-min(currentBits,nearestBits)<=1u;
        excludedLayer=index.y==1u?frontLayer:!frontLayer;
    }
    Draw draw=draws[index.x];uint flags=draw.flags.x;vec4 base=draw.baseColor;
    if((flags&1u)!=0u)base*=texture(sampler2D(baseTexture,materialSampler),uv);
    vec2 mr=draw.material.xy;if((flags&2u)!=0u)mr*=texture(sampler2D(materialTexture,materialSampler),uv).bg;
    float metal=clamp(mr.x,0,1),rough=clamp(mr.y,.04,1),transmission=clamp(draw.optics.x,0,1)*(1-metal);
    if((flags&64u)!=0u)
    {
        if(gl_FragCoord.z>opaqueDepth || excludedLayer)discard;
        float coverage=mix(clamp(base.a,0,1),1,transmission);
        outputColor=vec4(draw.baseColor.rgb*coverage,coverage);reactive=vec4(coverage);return;
    }
    if((flags&128u)!=0u)
    {
        if(gl_FragCoord.z>opaqueDepth || excludedLayer)discard;
        float coverage=clamp(base.a,0,1);
        outputColor=vec4(clamp(base.rgb,vec3(0),vec3(65504))*coverage,coverage);reactive=vec4(coverage);return;
    }
    vec3 geometricNormal=TransparentPixelNormalizeOr(worldNormal,vec3(0,0,1)),n=geometricNormal;
    vec3 v=TransparentPixelNormalizeOr(frame.camera.xyz-worldPosition,geometricNormal);
    if((flags&4u)!=0u)
    {
        vec3 fallback=normalize(cross(abs(n.z)<.99?vec3(0,0,1):vec3(0,1,0),n));
        vec3 t=TransparentPixelNormalizeOr(worldTangent.xyz-n*dot(n,worldTangent.xyz),fallback),b=cross(n,t)*worldTangent.w;
        vec3 mapped=texture(sampler2D(normalTexture,materialSampler),uv).xyz*2-1;
        mapped.xy*=draw.material.z;mapped.y*=(draw.emissive.w<0?-1.0:1.0);
        n=TransparentPixelNormalizeOr(mat3(t,b,n)*mapped,n);
    }
    // Filter the GGX lobe by the pixel's normal footprint. Evaluate derivatives
    // before discarding helper lanes or selecting a visible dielectric side.
    vec3 normalDx=dFdx(n),normalDy=dFdy(n);
    float normalVariance=0.25*(dot(normalDx,normalDx)+dot(normalDy,normalDy));
    float filteredAlphaSquared=min(1.0,rough*rough*rough*rough+min(2.0*normalVariance,0.18));
    float specularRoughness=sqrt(sqrt(filteredAlphaSquared));
    if(gl_FragCoord.z>opaqueDepth || excludedLayer)discard;
    if(transmission>0)
    {
        // This pass models a thin wall with explicit thickness, not two traced
        // dielectric interfaces. Orient the geometric and mapped normals on the
        // same visible side; an unsorted backface is not an inside-medium ray.
        if(dot(geometricNormal,v)<0) {geometricNormal=-geometricNormal;n=-n;}
        if(dot(n,v)<=0)n=geometricNormal;
    }
    else if(draw.flags.y!=0u && !gl_FrontFacing)n=-n;
    float ior=draw.optics.y,dielectric=pow((ior-1)/(ior+1),2),nv=max(dot(n,v),0);
    vec3 transmissionRay=transmission>0?refract(-v,n,1/ior):vec3(0);
    bool totalInternalReflection=transmission>0 && dot(transmissionRay,transmissionRay)<1e-6;
    vec3 f0=totalInternalReflection?vec3(1):mix(vec3(dielectric),base.rgb,metal),f=Fresnel(nv,f0);
    float ao=1;if((flags&8u)!=0u)ao=mix(1,texture(sampler2D(aoTexture,materialSampler),uv).r,draw.material.w);
    vec3 lit=(1-f)*base.rgb*(1-metal)*(1-transmission)*Irradiance(worldPosition,n)*ao;
    if((uint(frame.screen.w+.5)&2u)!=0u)lit+=Radiance(worldPosition,reflect(-v,n),specularRoughness)*f*frame.specularColor.rgb;
    for(int i=0;i<clamp(int(lights.counts.x+.5),0,4);++i)
    {
        vec3 l=normalize(lights.directional[i].directionIntensity.xyz);
        float visibility=Visibility(1,i,worldPosition,n,l,flags);
        lit+=Direct(n,v,l,lights.directional[i].color.rgb*lights.directional[i].directionIntensity.w*visibility,
            base.rgb,f0,specularRoughness,(1-metal)*(1-transmission));
    }
    uint clusterBase=ClusterBase(worldPosition),listed=0xffffffffu;
    if(clusterBase!=0xffffffffu)listed=clusterList[clusterBase];
    uint localCount=listed==0xffffffffu?clusters.counts.x:listed;
    for(uint item=0u;item<localCount;++item)
    {
        uint lightIndex=listed==0xffffffffu?item:clusterList[clusterBase+1u+item];
        ClusterLight light=clusterLights[lightIndex];
        vec3 delta=light.positionRange.xyz-worldPosition;float distance=length(delta);
        if(distance<=1e-4 || distance>=light.positionRange.w)continue;
        vec3 l=delta/distance;
        int type=int(light.coneShadow.y+.5),shadowIndex=int(light.coneShadow.z);
        float cone=1;
        if(type==3)cone=smoothstep(light.directionOuterCos.w,max(light.coneShadow.x,light.directionOuterCos.w+1e-4),
            dot(-l,light.directionOuterCos.xyz));
        float visibility=shadowIndex>=0?Visibility(type,shadowIndex,worldPosition,n,l,flags):1;
        lit+=Direct(n,v,l,light.colorIntensity.rgb*light.colorIntensity.w*Falloff(distance,light.positionRange.w)*cone*visibility,
            base.rgb,f0,specularRoughness,(1-metal)*(1-transmission));
    }
    for(int i=0;i<clamp(int(lights.areaCounts.x+.5),0,4);++i)
        lit+=EvaluateRectAreaLight(lights.rectAreaLights[i],n,v,base.rgb,f0,(1-metal)*(1-transmission),specularRoughness,worldPosition);
    for(int i=0;i<clamp(int(lights.areaCounts.y+.5),0,4);++i)
        lit+=EvaluateDiscAreaLight(lights.discAreaLights[i],n,v,base.rgb,f0,(1-metal)*(1-transmission),specularRoughness,worldPosition);
    vec3 refracted=vec3(0);
    if(transmission>0 && !totalInternalReflection)
    {
        vec3 ray=transmissionRay;
        float path=draw.optics.z/max(abs(dot(n,ray)),.1);vec4 exitClip=frame.viewProjection*vec4(worldPosition+ray*path,1);
        vec2 target=exitClip.xy/max(exitClip.w,1e-5)*vec2(.5,-.5)+.5;
        float exitDepth=exitClip.z/max(exitClip.w,1e-5);
        bool onScreen=exitClip.w>0 && exitDepth>=0 && exitDepth<=1 &&
            all(greaterThanEqual(target,vec2(0))) && all(lessThanEqual(target,vec2(1)));
        if(onScreen && textureLod(sampler2D(depthTexture,pointSampler),target,0).r<exitDepth)target=screenUV;
        refracted=onScreen?textureLod(sampler2D(opaqueTexture,screenSampler),target,0).rgb:Radiance(worldPosition,ray,rough);
        vec3 absorption=draw.optics.w>0?pow(clamp(draw.attenuation.rgb,vec3(.0001),vec3(1)),vec3(path/draw.optics.w)):vec3(1);
        refracted*=absorption*base.rgb*(1-f)*transmission;
    }
    vec3 emissive=draw.emissive.rgb;if((flags&16u)!=0u)emissive*=texture(sampler2D(emissiveTexture,materialSampler),uv).rgb;
    float coverage=mix(clamp(base.a,0,1),1,transmission);
    vec3 color=lights.counts.w>.5?lit+refracted+emissive:base.rgb+emissive;
    outputColor=vec4(clamp(color,vec3(0),vec3(65504))*coverage,coverage);
    reactive=vec4(coverage);
}
