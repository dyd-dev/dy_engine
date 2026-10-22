#include <metal_stdlib>
// 이 셰이더가 사용하는 바인딩과 입력 크기다.
#ifndef RENDERER_BINDLESS
#define RENDERER_BINDLESS 0
#endif
#ifndef RENDERER_DEPTH_PREPASS
#define RENDERER_DEPTH_PREPASS 0
#endif
#ifndef RENDERER_ENABLE_AREA_LIGHTS
#define RENDERER_ENABLE_AREA_LIGHTS 1
#endif
#define RENDERER_MAX_DIRECTIONAL_LIGHTS 4
#define RENDERER_MAX_POINT_LIGHTS 16
#define RENDERER_MAX_SPOT_LIGHTS 16
#define RENDERER_MAX_RECT_AREA_LIGHTS 4
#define RENDERER_MAX_DISC_AREA_LIGHTS 4

#define RENDERER_BINDING_BASE_COLOR_TEXTURE 0
#define RENDERER_BINDING_LIGHTING_CONSTANTS 1
#if RENDERER_BINDLESS
#define RENDERER_BINDING_SHADOW_TEXTURE 28
#else
#define RENDERER_BINDING_SHADOW_TEXTURE 2
#endif
#define RENDERER_BINDING_SHADOW_MATRIX 3
#define RENDERER_BINDING_METALLIC_ROUGHNESS_TEXTURE 4
#define RENDERER_BINDING_NORMAL_TEXTURE 5
#define RENDERER_BINDING_OCCLUSION_TEXTURE 6
#define RENDERER_BINDING_EMISSIVE_TEXTURE 7
#define RENDERER_BINDING_MATERIAL_SAMPLER 8
#define RENDERER_BINDING_SHADOW_SAMPLER 9
#define RENDERER_BINDING_INLINE_CONSTANTS 10

#define RENDERER_TEXTURE_FLAG_BASE_COLOR 1
#define RENDERER_TEXTURE_FLAG_METALLIC_ROUGHNESS 2
#define RENDERER_TEXTURE_FLAG_NORMAL 4
#define RENDERER_TEXTURE_FLAG_OCCLUSION 8
#define RENDERER_TEXTURE_FLAG_EMISSIVE 16
#define RENDERER_TEXTURE_FLAG_RECEIVE_SHADOW 32

#ifndef RENDERER_ENABLE_SHADOWS
#error RENDERER_ENABLE_SHADOWS must be defined
#endif

#ifndef RENDERER_FRAGMENT_ENTRY
#error RENDERER_FRAGMENT_ENTRY must be defined
#endif

using namespace metal;
// Basic bindless rendering supplies neutral settings.x=0 and screen.z=0.
struct EnvironmentConstants
{
    float4 sh[9];
    float4 settings;
    float4 screen;
    float4 probePosition;
    float4 probeBoxMin;
    float4 probeBoxMax;
};
inline float3 EnvironmentIrradiance(float3 n,constant EnvironmentConstants& environment)
{
    const float c=cos(environment.settings.z),s=sin(environment.settings.z);
    n.xy=float2(c*n.x+s*n.y,-s*n.x+c*n.y);
    const float3 value=environment.sh[0].rgb*0.28209479177387814f+
        environment.sh[1].rgb*(0.4886025119029199f*n.y)+environment.sh[2].rgb*(0.4886025119029199f*n.z)+
        environment.sh[3].rgb*(0.4886025119029199f*n.x)+environment.sh[4].rgb*(1.0925484305920792f*n.x*n.y)+
        environment.sh[5].rgb*(1.0925484305920792f*n.y*n.z)+environment.sh[6].rgb*(0.31539156525252005f*(3*n.z*n.z-1))+
        environment.sh[7].rgb*(1.0925484305920792f*n.x*n.z)+environment.sh[8].rgb*(0.5462742152960396f*(n.x*n.x-n.y*n.y));
    return max(value,0.0f);
}

struct DrawConstants
{
    float4x4 viewProjectionMatrix;
    float4x4 modelMatrix;
    uint textureFlags;
    uint padding0;
    uint padding1;
    uint padding2;
    float4 emissiveColor;
    float4 baseColor;
    float4 materialParams;
};

// CPU가 업로드하는 조명 버퍼의 필드 순서와 크기를 유지한다.
struct RendererDirectionalLight
{
	float4 directionIntensity;
	float4 color;
};

struct RendererPointLight
{
	float4 positionRange;
	float4 colorIntensity;
};

struct RendererSpotLight
{
	float4 positionRange;
	float4 directionOuterCos;
	float4 colorIntensity;
	float4 coneParams;
};

struct RendererRectAreaLight
{
	float4 positionIntensity;
	float4 directionWidth;
	float4 upHeight;
	float4 color;
};

struct RendererDiscAreaLight
{
	float4 positionIntensity;
	float4 directionRadius;
	float4 up;
	float4 color;
};

struct RendererLightingConstants
{
	float4 cameraPosition;
	float4 directionalLightDirection;
	float4 directionalLightColor;
	float4 ambientColor;
	float4 shadowParams;
	float4 pbrParams;
	float4 environmentColor;
	float4 pointLightPositionRange;
	float4 pointLightColorIntensity;
	float4 lightCounts;
	float4 areaLightCounts;
	RendererDirectionalLight directionalLights[RENDERER_MAX_DIRECTIONAL_LIGHTS];
	RendererPointLight pointLights[RENDERER_MAX_POINT_LIGHTS];
	RendererSpotLight spotLights[RENDERER_MAX_SPOT_LIGHTS];
	RendererRectAreaLight rectAreaLights[RENDERER_MAX_RECT_AREA_LIGHTS];
	RendererDiscAreaLight discAreaLights[RENDERER_MAX_DISC_AREA_LIGHTS];
	float4 shadowLight;
};

struct RasterData
{
    float4 position [[position]];
    float2 uv [[user(locn0)]];
    float3 worldPosition [[user(locn1)]];
    float3 worldNormal [[user(locn2)]];
    float4 worldTangent [[user(locn3)]];
#if RENDERER_ENABLE_SHADOWS
    float4 lightSpacePosition [[user(locn4)]];
#endif
};

constant uint kTextureFlagBaseColor =
    uint(RENDERER_TEXTURE_FLAG_BASE_COLOR);
constant uint kTextureFlagMetallicRoughness =
    uint(RENDERER_TEXTURE_FLAG_METALLIC_ROUGHNESS);
constant uint kTextureFlagNormal =
    uint(RENDERER_TEXTURE_FLAG_NORMAL);
constant uint kTextureFlagOcclusion =
    uint(RENDERER_TEXTURE_FLAG_OCCLUSION);
constant uint kTextureFlagEmissive =
    uint(RENDERER_TEXTURE_FLAG_EMISSIVE);
constant float kPi = 3.14159265359f;

inline float DistributionGGX(float3 normal, float3 halfway, float roughness)
{
    const float a = roughness * roughness;
    const float a2 = a * a;
    const float ndoth = clamp(dot(normal, halfway), 0.0f, 1.0f);
    const float3 crossNH = cross(normal, halfway);
    // Avoid cancellation in 1 - (N.H)^2 near a narrow specular peak.
    const float denominator = dot(crossNH, crossNH) + ndoth * ndoth * a2;
    return a2 / (kPi * denominator * denominator);
}

inline float GeometrySchlickGGX(float ndotv, float roughness)
{
    const float r = roughness + 1.0f;
    const float k = (r * r) / 8.0f;
    return ndotv / max(ndotv * (1.0f - k) + k, 0.0001f);
}

inline float3 FresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0f - f0) *
        pow(saturate(1.0f - cosTheta), 5.0f);
}

inline float3 FresnelSchlickRoughness(
    float cosTheta,
    float3 f0,
    float roughness)
{
    const float3 roughnessF0 = max(float3(1.0f - roughness), f0);
    return f0 + (roughnessF0 - f0) *
        pow(saturate(1.0f - cosTheta), 5.0f);
}

#if RENDERER_ENABLE_SHADOWS
struct ShadowMatrix {
    float4x4 lightViewProjectionMatrix[128];
    float4 atlasRect[128];
    float4 directionalViews[RENDERER_MAX_DIRECTIONAL_LIGHTS];
    float4 pointViews[RENDERER_MAX_POINT_LIGHTS];
    float4 spotViews[RENDERER_MAX_SPOT_LIGHTS];
    float4 directionalSplits[RENDERER_MAX_DIRECTIONAL_LIGHTS];
    float4x4 cameraView;
    float4 filterParams;
    float4 viewProjectionParams[128];
    float4 softShadowParams;
};

#define DY_SHADOW_STRUCT ShadowMatrix
#define DY_SHADOW_TEXTURE depth2d<float>
#define DY_SHADOW_PROJECTION(index) shadowData.viewProjectionParams[index]
#define DY_SHADOW_MATRIX(index) shadowData.lightViewProjectionMatrix[index]
#define DY_SHADOW_TILE(index) shadowData.atlasRect[index]
#define DY_SHADOW_DIMENSIONS int2(shadowImage.get_width(),shadowImage.get_height())
#define DY_SHADOW_READ(pixel) shadowImage.read(uint2(pixel))
#define DY_SHADOW_FILTER shadowData.filterParams
#define DY_SHADOW_SOFT shadowData.softShadowParams
#include "shadow_filter.metal"

float SampleShadowView(int index,float3 worldPosition,float3 normal,float3 lightDir,float strength,float sourceRadius,int pointBase,
    constant RendererLightingConstants& lighting,constant ShadowMatrix& shadowMatrix,depth2d<float> shadowMap,sampler shadowSampler)
{
    float4 clip=shadowMatrix.lightViewProjectionMatrix[index]*float4(worldPosition,1.0);
    if(clip.w<=0.0)return 1.0;
    float3 ndc=clip.xyz/clip.w;
    float2 uv=ndc.xy*.5+.5;
    uv.y=1.0-uv.y;
    if(uv.x<0.0||uv.y<0.0||uv.x>1.0||uv.y>1.0||ndc.z<0.0||ndc.z>1.0)return 1.0;
    float4 tile=shadowMatrix.atlasRect[index];
    float2 texel=1.0/float2(shadowMap.get_width(),shadowMap.get_height());
    float2 minUv=tile.xy+texel*.5;
    float2 maxUv=tile.xy+tile.zw-texel*.5;
    float2 localUv=uv;
    uv=tile.xy+uv*tile.zw;
    float angle=1.0-max(dot(normal,lightDir),0.0);
    float bias=max(lighting.shadowParams.x,lighting.shadowParams.y*angle)+lighting.shadowParams.z*angle;
    float receiver=ndc.z-bias;
    if(sourceRadius>0 && shadowMatrix.softShadowParams.x>0)
        return 1-clamp(strength,0.0,1.0)*(1-DyShadowPcss(index,pointBase,localUv,receiver,clip.w,lightDir,sourceRadius,shadowMatrix,shadowMap));
    float radius=min(lighting.shadowParams.w,shadowMatrix.filterParams.z);
    int extent=int(ceil(radius));
    float visible=0.0,samples=0.0;
    for(int y=-extent;y<=extent;++y)for(int x=-extent;x<=extent;++x)
    {
        float2 sampleUv=clamp(uv+float2(x,y)*texel,minUv,maxUv);
        visible+=receiver<=shadowMap.sample(shadowSampler,sampleUv,level(0))?1.0:0.0;
        samples+=1.0;
    }
    return 1.0-clamp(strength,0.0,1.0)*(1.0-visible/max(samples,1.0));
}

float SampleShadow(int type,int lightIndex,float3 worldPosition,float3 normal,float3 lightDir,
    constant RendererLightingConstants& lighting,constant ShadowMatrix& shadowMatrix,depth2d<float> shadowMap,sampler shadowSampler,uint drawFlags)
{
    if((drawFlags & RENDERER_TEXTURE_FLAG_RECEIVE_SHADOW)==0u)return 1.0;
    float4 views=type==1?shadowMatrix.directionalViews[lightIndex]:(type==2?shadowMatrix.pointViews[lightIndex]:shadowMatrix.spotViews[lightIndex]);
    int count=int(views.y+.5);
    if(count==0)return 1.0;
    int local=0;
    float depth=0.0;
    if(type==1)
    {
        depth=-(shadowMatrix.cameraView*float4(worldPosition,1.0)).z;
        float finalSplit=shadowMatrix.directionalSplits[lightIndex][count-1];
        if(finalSplit>0 && depth>finalSplit)return 1.0;
        while(local<count-1 && depth>shadowMatrix.directionalSplits[lightIndex][local])++local;
    }
    if(type==2)
    {
        float3 direction=-lightDir, magnitude=abs(direction);
        if(magnitude.x>=magnitude.y && magnitude.x>=magnitude.z)local=direction.x>=0?0:1;
        else if(magnitude.y>=magnitude.z)local=direction.y>=0?2:3;
        else local=direction.z>=0?4:5;
    }
    int index=int(views.x+.5)+local;
    float visibility=SampleShadowView(index,worldPosition,normal,lightDir,views.z,views.w,type==2?int(views.x+.5):-1,lighting,shadowMatrix,shadowMap,shadowSampler);
    if(type==1 && local<count-1)
    {
        float previous=local==0?shadowMatrix.filterParams.w:shadowMatrix.directionalSplits[lightIndex][local-1];
        float split=shadowMatrix.directionalSplits[lightIndex][local];
        float blend=smoothstep(mix(previous,split,.9f),split,depth);
        if(blend>0.0)visibility=mix(visibility,
            SampleShadowView(index+1,worldPosition,normal,lightDir,views.z,views.w,type==2?int(views.x+.5):-1,lighting,shadowMatrix,shadowMap,shadowSampler),blend);
    }
    return visibility;
}

#endif

// 현재 언어의 조명 계산을 직접 작성한다.
inline float PunctualAttenuation(float distanceToLight, float range) {
    if (range <= 0.0 || distanceToLight >= range) {
        return 0.0;
    }
    float distanceSquared = max(distanceToLight * distanceToLight, 0.0001);
    float ratio = distanceToLight / range;
    float ratioSquared = ratio * ratio;
    float window = max(1.0 - ratioSquared * ratioSquared, 0.0);
    return (window * window) / distanceSquared;
}

inline float SpotConeAttenuation(float cosTheta, float innerCos, float outerCos) {
    float denominator = max(innerCos - outerCos, 0.0001);
    float t = clamp((cosTheta - outerCos) / denominator, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

inline float3 EvaluateDirectLight(
    float3 normal,
    float3 viewDir,
    float3 lightDir,
    float3 incidentIlluminance,
    float3 albedo,
    float metallic,
    float roughness,
    float visibility) {
    float ndotl = max(dot(normal, lightDir), 0.0);
    float ndotv = max(dot(normal, viewDir), 0.0);
    if (ndotl <= 0.0 || ndotv <= 0.0 || visibility <= 0.0) {
        return float3(0.0,0.0,0.0);
    }
    float3 halfwayVector = viewDir + lightDir;
    float halfwayLengthSquared = dot(halfwayVector, halfwayVector);
    if (halfwayLengthSquared <= 0.000001) {
        return float3(0.0,0.0,0.0);
    }
    float3 halfway = halfwayVector * rsqrt(halfwayLengthSquared);
    float3 f0 = mix(float3(0.04,0.04,0.04), albedo, metallic);
    float ndf = DistributionGGX(normal, halfway, roughness);
    float geometry = GeometrySchlickGGX(ndotv, roughness) * GeometrySchlickGGX(ndotl, roughness);
    float3 fresnel = FresnelSchlick(max(dot(halfway, viewDir), 0.0), f0);
    float3 specular = (ndf * geometry * fresnel) / max(4.0 * ndotv * ndotl, 0.0001);
    float3 diffuseWeight = (float3(1.0,1.0,1.0) - fresnel) * (1.0 - metallic);
    return (diffuseWeight * albedo / kPi + specular) * incidentIlluminance * ndotl * visibility;
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
    float3 albedo,float metallic,float roughness,float3 radiance,texture2d<float> ltcMatrixTexture,texture2d<float> ltcAmplitudeTexture,sampler materialSampler)
{
    float ndotv=clamp(dot(normal,viewDir),0.0,1.0);
    if(ndotv<=0)return float3(0);
    float2 coordinate=(float2(clamp(roughness,0.0,1.0),sqrt(1-ndotv))*63.0+.5)/64.0;
    float4 inverseMatrix=ltcMatrixTexture.sample(materialSampler,coordinate,level(0));
    float4 amplitude=ltcAmplitudeTexture.sample(materialSampler,coordinate,level(0));
    float specular=LtcPolygonIntegral(normal,viewDir,position,inverseMatrix,points,count);
    float diffuse=LtcPolygonIntegral(normal,viewDir,position,float4(1,0,0,1),points,count);
    float3 f0=mix(float3(.04),albedo,metallic);
    float3 specularWeight=f0*max(amplitude.x,0.0)+(float3(1)-f0)*max(amplitude.y,0.0);
    float3 diffuseWeight=(float3(1)-FresnelSchlickRoughness(ndotv,f0,roughness))*(1-metallic);
    // 정규화된 cosine 적분이므로 diffuse에 1/pi나 거리 감쇠를 다시 곱하지 않는다.
    return radiance*(specularWeight*specular+diffuseWeight*albedo*diffuse);
}
inline float3 EvaluateRectAreaLight(RendererRectAreaLight light,float3 normal,float3 viewDir,
    float3 albedo,float metallic,float roughness,float3 worldPosition,texture2d<float> ltcMatrixTexture,texture2d<float> ltcAmplitudeTexture,sampler materialSampler)
{
    float3 direction=normalize(light.directionWidth.xyz);
    if(light.directionWidth.w<=0 || light.upHeight.w<=0 || dot(direction,worldPosition-light.positionIntensity.xyz)<=0)
        return float3(0);
    float3 right=normalize(cross(direction,light.upHeight.xyz));
    float3 up=normalize(cross(right,direction));
    float3 x=right*(light.directionWidth.w*.5),y=up*(light.upHeight.w*.5),center=light.positionIntensity.xyz;
    float3 points[16];
    points[0]=center-x-y;points[1]=center+x-y;points[2]=center+x+y;points[3]=center-x+y;
    return EvaluateLtcPolygon(normal,viewDir,worldPosition,points,4,albedo,metallic,roughness,
        light.color.rgb*max(light.positionIntensity.w,0.0),ltcMatrixTexture,ltcAmplitudeTexture,materialSampler);
}
inline float3 EvaluateDiscAreaLight(RendererDiscAreaLight light,float3 normal,float3 viewDir,
    float3 albedo,float metallic,float roughness,float3 worldPosition,texture2d<float> ltcMatrixTexture,texture2d<float> ltcAmplitudeTexture,sampler materialSampler)
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
    return EvaluateLtcPolygon(normal,viewDir,worldPosition,points,16,albedo,metallic,roughness,
        light.color.rgb*max(light.positionIntensity.w,0.0),ltcMatrixTexture,ltcAmplitudeTexture,materialSampler);
}
#endif

struct ClusterLight {float4 positionRange;float4 colorIntensity;float4 directionOuterCos;float4 coneShadow;};
struct ClusterParameters {float4x4 view;float4x4 projection;float4x4 inverseProjection;float4 depth;uint4 dimensions;uint4 counts;};
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

inline float3 EvaluateLights(constant RendererLightingConstants& lighting,
    const device ClusterLight* clusterLights,const device uint* clusterList,constant ClusterParameters& clusters,
    texture2d<float> ltcMatrixTexture,texture2d<float> ltcAmplitudeTexture,sampler materialSampler,
#if RENDERER_ENABLE_SHADOWS
    constant ShadowMatrix& shadowMatrix, depth2d<float> shadowMap, sampler shadowSampler, uint drawFlags,
#endif
    float3 worldPosition, float3 normal, float3 viewDir,
    float3 albedo, float metallic, float roughness) {
    float3 directLight = float3(0.0,0.0,0.0);
    float ndotv=max(dot(normal,viewDir),0.0f);
    int directionalCount = clamp(int(lighting.lightCounts.x + 0.5), 0, RENDERER_MAX_DIRECTIONAL_LIGHTS);
    for (int lightIndex = 0; lightIndex < directionalCount; ++lightIndex) {
        RendererDirectionalLight light = lighting.directionalLights[lightIndex];
        float3 lightDir = normalize(light.directionIntensity.xyz);
        // Match EvaluateDirectLight's zero result before sampling its shadow.
        if(ndotv<=0.0f || max(dot(normal,lightDir),0.0f)<=0.0f)continue;
#if RENDERER_ENABLE_SHADOWS
        float visibility = SampleShadow(1,lightIndex,worldPosition,normal,lightDir,lighting,shadowMatrix,shadowMap,shadowSampler,drawFlags);
#else
        float visibility = 1.0;
#endif
        directLight += EvaluateDirectLight(normal, viewDir, lightDir,
            light.color.rgb * max(light.directionIntensity.w, 0.0), albedo, metallic, roughness, visibility);
    }
    uint base=ClusterBase(worldPosition,clusters);
    uint listed=0xffffffffu;
    if(base!=0xffffffffu)listed=clusterList[base];
    uint localCount=listed==0xffffffffu?clusters.counts.x:listed;
    for(uint item=0u;item<localCount;++item)
    {
        uint lightIndex=listed==0xffffffffu?item:clusterList[base+1u+item];
        ClusterLight light=clusterLights[lightIndex];
        float3 toLight=light.positionRange.xyz-worldPosition;
        float distanceToLight=length(toLight);
        if(distanceToLight<=0.0001 || distanceToLight>=light.positionRange.w)continue;
        float3 lightDir=toLight/distanceToLight;
        if(ndotv<=0.0f || max(dot(normal,lightDir),0.0f)<=0.0f)continue;
        float attenuation=PunctualAttenuation(distanceToLight,light.positionRange.w);
        int type=int(light.coneShadow.y+0.5),shadowIndex=int(light.coneShadow.z);
        if(type==3)attenuation*=SpotConeAttenuation(dot(-lightDir,light.directionOuterCos.xyz),
            light.coneShadow.x,light.directionOuterCos.w);
#if RENDERER_ENABLE_SHADOWS
        float visibility=shadowIndex>=0?SampleShadow(type,shadowIndex,worldPosition,normal,lightDir,lighting,shadowMatrix,shadowMap,shadowSampler,drawFlags):1.0;
#else
        float visibility=1.0;
#endif
        float3 incidentIlluminance=light.colorIntensity.rgb*max(light.colorIntensity.w,0.0)*attenuation;
        directLight+=EvaluateDirectLight(normal,viewDir,lightDir,incidentIlluminance,albedo,metallic,roughness,visibility);
    }
#if RENDERER_ENABLE_AREA_LIGHTS
    int rectAreaCount = clamp(int(lighting.areaLightCounts.x + 0.5), 0, RENDERER_MAX_RECT_AREA_LIGHTS);
    for (int lightIndex = 0; lightIndex < rectAreaCount; ++lightIndex) {
        directLight += EvaluateRectAreaLight(lighting.rectAreaLights[lightIndex], normal, viewDir, albedo, metallic, roughness, worldPosition,
            ltcMatrixTexture,ltcAmplitudeTexture,materialSampler);
    }
    int discAreaCount = clamp(int(lighting.areaLightCounts.y + 0.5), 0, RENDERER_MAX_DISC_AREA_LIGHTS);
    for (int lightIndex = 0; lightIndex < discAreaCount; ++lightIndex) {
        directLight += EvaluateDiscAreaLight(lighting.discAreaLights[lightIndex], normal, viewDir, albedo, metallic, roughness, worldPosition,
            ltcMatrixTexture,ltcAmplitudeTexture,materialSampler);
    }
#endif

    return directLight;
}

inline float3 ResolveNormal(
    RasterData input,
    constant DrawConstants& drawConstants,
    texture2d<float> normalTexture,
    sampler materialSampler)
{
    float3 normal = normalize(input.worldNormal);
    const uint textureFlags = drawConstants.textureFlags;
    if ((textureFlags & kTextureFlagNormal) == 0u ||
        abs(drawConstants.materialParams.z) <= 0.0001f)
    {
        return normal;
    }

    const float3 tangent = normalize(
        input.worldTangent.xyz -
        normal * dot(normal, input.worldTangent.xyz));
    const float3 bitangent =
        normalize(cross(normal, tangent)) * input.worldTangent.w;
    const float3x3 tangentBasis = float3x3(tangent, bitangent, normal);
    float3 tangentNormal =
        normalTexture.sample(materialSampler, input.uv).xyz * 2.0f - 1.0f;
    tangentNormal.xy *= drawConstants.materialParams.z;
    if((drawConstants.padding1 & 8u)!=0u) tangentNormal.y=-tangentNormal.y;
    return normalize(tangentBasis * tangentNormal);
}

inline float4 RunFragmentShader(
    RasterData input,
    bool frontFace,
    constant DrawConstants& drawConstants,
    constant RendererLightingConstants& lighting,
    const device ClusterLight* clusterLights,const device uint* clusterList,constant ClusterParameters& clusters,
    texture2d<float> ltcMatrixTexture,texture2d<float> ltcAmplitudeTexture,
    texture2d<float> baseColorTexture,
#if RENDERER_ENABLE_SHADOWS
    depth2d<float> shadowMap,
    constant ShadowMatrix& shadowMatrix,
#endif
    texture2d<float> metallicRoughnessTexture,
    texture2d<float> normalTexture,
    texture2d<float> occlusionTexture,
    texture2d<float> emissiveTexture,
#if RENDERER_ENABLE_SHADOWS
    sampler shadowSampler,
#endif
    sampler materialSampler,
    texture2d<float> screenAOTexture,texture2d<float> environmentSpecularTexture,texture2d<float> environmentDiffuseTexture,
    constant EnvironmentConstants& environment,sampler screenSampler
    )
{
    const uint textureFlags = drawConstants.textureFlags;
    float4 base = drawConstants.baseColor;
    if ((textureFlags & kTextureFlagBaseColor) != 0u)
    {
        base *= baseColorTexture.sample(materialSampler, input.uv);
    }
    const uint alphaMode=drawConstants.padding1 & 3u;
    if((textureFlags&64u)!=0u)
    {
        if(alphaMode==1u && base.a<as_type<float>(drawConstants.padding0)) discard_fragment();
        float3 color=drawConstants.baseColor.rgb;
        if(lighting.pbrParams.z>.5f) color=pow(color,float3(1.0f/2.2f));
        return float4(color,alphaMode==2u ? base.a : 1.0f);
    }
    if((drawConstants.padding1&16u)!=0u)
    {
        if(alphaMode==1u && base.a<as_type<float>(drawConstants.padding0)) discard_fragment();
        float3 color=base.rgb;
        if(lighting.pbrParams.z>=0.0f)
        {
            color=min(color*lighting.pbrParams.w,float3(16777216.0f));
            color=color/(color+1.0f);
            if(lighting.pbrParams.z>.5f)color=pow(color,float3(1.0f/2.2f));
        }
        color=float3(isnan(color.r)?0:clamp(color.r,0.0f,65504.0f),isnan(color.g)?0:clamp(color.g,0.0f,65504.0f),
            isnan(color.b)?0:clamp(color.b,0.0f,65504.0f));
        return float4(color,alphaMode==2u?base.a:1.0f);
    }
    const float3 albedo=base.rgb;

    const float minRoughness =
        clamp(lighting.pbrParams.x, 0.01f, 1.0f);
    const float ambientSpecularStrength =
        max(lighting.pbrParams.y, 0.0f);
    float metallic =
        clamp(drawConstants.materialParams.x, 0.0f, 1.0f);
    float roughness =
        clamp(drawConstants.materialParams.y, minRoughness, 1.0f);
    float occlusion = 1.0f;
    // Opaque environment buffers already include authored AO and screen AO.
    bool usesEnvironmentBuffers=environment.settings.x>0.5f && alphaMode!=2u;

    if ((textureFlags & kTextureFlagMetallicRoughness) != 0u)
    {
        const float4 metallicRoughness =
            metallicRoughnessTexture.sample(materialSampler, input.uv);
        roughness = clamp(
            roughness * metallicRoughness.g,
            minRoughness,
            1.0f);
        metallic = clamp(
            metallic * metallicRoughness.b,
            0.0f,
            1.0f);
    }
    if ((textureFlags & kTextureFlagOcclusion) != 0u && !usesEnvironmentBuffers)
    {
        occlusion = mix(1.0f, occlusionTexture.sample(materialSampler, input.uv).r,
            clamp(drawConstants.materialParams.w, 0.0f, 1.0f));
    }

    float3 normal =
        ResolveNormal(input, drawConstants, normalTexture, materialSampler);
    // Derivatives precede discard and the two-sided normal orientation branch.
    const float3 normalDx=dfdx(normal),normalDy=dfdy(normal);
    const float normalVariance=0.25f*(dot(normalDx,normalDx)+dot(normalDy,normalDy));
    const float filteredAlphaSquared=min(1.0f,roughness*roughness*roughness*roughness+min(2.0f*normalVariance,0.18f));
    roughness=sqrt(sqrt(filteredAlphaSquared));
    if(alphaMode==1u && base.a<as_type<float>(drawConstants.padding0)) discard_fragment();
    if((drawConstants.padding1 & 4u)!=0u && !frontFace) normal=-normal;
    const float3 viewDirection =
        normalize(lighting.cameraPosition.xyz - input.worldPosition);

    const float3 f0 =
        mix(float3(0.04f), albedo, metallic);
    const float3 directLight = EvaluateLights(lighting,
        clusterLights,clusterList,clusters,
        ltcMatrixTexture,ltcAmplitudeTexture,materialSampler,
#if RENDERER_ENABLE_SHADOWS
        shadowMatrix,shadowMap,shadowSampler,drawConstants.textureFlags,
#endif
        input.worldPosition, normal, viewDirection, albedo, metallic, roughness);

    const float3 ambientFresnel = FresnelSchlickRoughness(
        max(dot(normal, viewDirection), 0.0f),
        f0,
        roughness);
    const float3 kD = (float3(1.0f) - ambientFresnel) * (1.0f - metallic);
    const float3 ambientDiffuse =
        kD *
        albedo *
        lighting.ambientColor.rgb *
        lighting.ambientColor.a;
    const float3 ambientSpecular =
        ambientFresnel *
        lighting.environmentColor.rgb *
        lighting.environmentColor.a *
        ambientSpecularStrength;
    float3 ambient =
        (ambientDiffuse + ambientSpecular) * occlusion;
    const float2 screenUV=input.position.xy*environment.screen.xy;
    const float screenAO=!usesEnvironmentBuffers && environment.screen.z>0.5 ? screenAOTexture.sample(screenSampler,screenUV,level(0)).r : 1.0f;
    ambient*=screenAO;
    if(environment.settings.x>0.5)
    {
        ambient=0;
        if(alphaMode==2u)
        {
            if((uint(environment.screen.w+0.5f)&1u)!=0u)
                ambient=kD*albedo*EnvironmentIrradiance(normal,environment)*(0.3183098861837907f*environment.settings.y*occlusion*screenAO);
        }
        else
        {
            ambient=kD*albedo*environmentDiffuseTexture.sample(screenSampler,screenUV,level(0)).rgb;
            ambient+=environmentSpecularTexture.sample(screenSampler,screenUV,level(0)).rgb;
        }
    }

    float3 emissive = drawConstants.emissiveColor.rgb;
    if ((textureFlags & kTextureFlagEmissive) != 0u)
    {
        emissive *=
            emissiveTexture.sample(materialSampler, input.uv).rgb;
    }

    float3 color = lighting.lightCounts.w > 0.5 ? ambient + directLight + emissive : albedo + emissive;
    if(lighting.pbrParams.z >= 0.0f) {
        color = min(color * lighting.pbrParams.w, 16777216.0f);
        color = color / (color + 1.0f);
        if (lighting.pbrParams.z > 0.5f) color = pow(color, float3(1.0f / 2.2f));
    }
    // Preserve finite RGBA16F scene values before screen-space/temporal filtering.
    color=select(clamp(color,0.0f,65504.0f),float3(0),isnan(color));
    return float4(color, alphaMode==2u ? base.a : 1.0f);
}

#if RENDERER_DEPTH_PREPASS
[[early_fragment_tests]]
#endif
fragment float4 RENDERER_FRAGMENT_ENTRY(
    RasterData input [[stage_in]],
    bool frontFace [[front_facing]],
#if RENDERER_BINDLESS
    texture2d<float> screenAOTexture [[texture(22)]],
    texture2d<float> environmentSpecularTexture [[texture(23)]],
    texture2d<float> environmentDiffuseTexture [[texture(24)]],
#else
    texture2d<float> screenAOTexture [[texture(16)]],
    texture2d<float> environmentSpecularTexture [[texture(13)]],
    texture2d<float> environmentDiffuseTexture [[texture(17)]],
#endif
    constant EnvironmentConstants& environment [[buffer(14)]],
    sampler screenSampler [[sampler(15)]],
#if RENDERER_BINDLESS
    array<texture2d<float>,19> materialTextures [[texture(0)]],
    const device uint4* materialTextureIndices [[buffer(29)]],
#endif
    constant DrawConstants& drawConstants [[buffer(RENDERER_BINDING_INLINE_CONSTANTS)]],
    constant RendererLightingConstants& lighting [[buffer(RENDERER_BINDING_LIGHTING_CONSTANTS)]],
    const device ClusterLight* clusterLights [[buffer(19)]],
    const device uint* clusterList [[buffer(20)]],
    constant ClusterParameters& clusters [[buffer(21)]],
    texture2d<float> ltcMatrixTexture [[texture(26)]],
    texture2d<float> ltcAmplitudeTexture [[texture(27)]],
#if !RENDERER_BINDLESS
    texture2d<float> baseColorTexture [[texture(RENDERER_BINDING_BASE_COLOR_TEXTURE)]],
#endif
#if RENDERER_ENABLE_SHADOWS
    depth2d<float> shadowMap [[texture(RENDERER_BINDING_SHADOW_TEXTURE)]],
    constant ShadowMatrix& shadowMatrix [[buffer(RENDERER_BINDING_SHADOW_MATRIX)]],
#endif
#if !RENDERER_BINDLESS
    texture2d<float> metallicRoughnessTexture [[texture(RENDERER_BINDING_METALLIC_ROUGHNESS_TEXTURE)]],
#endif
#if !RENDERER_BINDLESS
    texture2d<float> normalTexture [[texture(RENDERER_BINDING_NORMAL_TEXTURE)]],
#endif
#if !RENDERER_BINDLESS
    texture2d<float> occlusionTexture [[texture(RENDERER_BINDING_OCCLUSION_TEXTURE)]],
#endif
#if !RENDERER_BINDLESS
    texture2d<float> emissiveTexture [[texture(RENDERER_BINDING_EMISSIVE_TEXTURE)]],
#endif
#if RENDERER_ENABLE_SHADOWS
    sampler shadowSampler [[sampler(RENDERER_BINDING_SHADOW_SAMPLER)]],
#endif
    sampler materialSampler [[sampler(RENDERER_BINDING_MATERIAL_SAMPLER)]])
{
#if RENDERER_BINDLESS
    const uint4 indicesA=materialTextureIndices[drawConstants.padding2*2];
    const uint4 indicesB=materialTextureIndices[drawConstants.padding2*2+1];
#define baseColorTexture materialTextures[indicesA[0]]
#define metallicRoughnessTexture materialTextures[indicesA[1]]
#define normalTexture materialTextures[indicesA[2]]
#define occlusionTexture materialTextures[indicesA[3]]
#define emissiveTexture materialTextures[indicesB.x]
#endif
    return RunFragmentShader(
        input,
        frontFace,
        drawConstants,
        lighting,
        clusterLights,clusterList,clusters,
        ltcMatrixTexture,ltcAmplitudeTexture,
        baseColorTexture,
#if RENDERER_ENABLE_SHADOWS
        shadowMap,
        shadowMatrix,
#endif
        metallicRoughnessTexture,
        normalTexture,
        occlusionTexture,
        emissiveTexture,
#if RENDERER_ENABLE_SHADOWS
        shadowSampler,
#endif
        materialSampler,
        screenAOTexture,environmentSpecularTexture,environmentDiffuseTexture,environment,screenSampler
        );
}
