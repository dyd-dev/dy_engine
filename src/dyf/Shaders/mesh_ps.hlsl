// mesh_ps.hlsl - D3D12 Renderer lighting path matching the Vulkan GLSL shader.

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

#define RENDERER_TEXTURE_FLAG_BASE_COLOR 1
#define RENDERER_TEXTURE_FLAG_METALLIC_ROUGHNESS 2
#define RENDERER_TEXTURE_FLAG_NORMAL 4
#define RENDERER_TEXTURE_FLAG_OCCLUSION 8
#define RENDERER_TEXTURE_FLAG_EMISSIVE 16
#define RENDERER_TEXTURE_FLAG_RECEIVE_SHADOW 32

#ifndef RENDERER_ENABLE_SHADOWS
#error RENDERER_ENABLE_SHADOWS must be defined
#endif


cbuffer DrawConstants : register(b10, space0)
{
    column_major float4x4 viewProjectionMatrix;
    column_major float4x4 modelMatrix;
    uint drawTextureFlags;
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
cbuffer RendererLighting : register(b1, space0) { RendererLightingConstants lighting; };
struct ClusterLight {float4 positionRange;float4 colorIntensity;float4 directionOuterCos;float4 coneShadow;};
StructuredBuffer<ClusterLight> clusterLights:register(t19,space0);
StructuredBuffer<uint> clusterList:register(t20,space0);
cbuffer ClusterParameters:register(b21,space0) {column_major float4x4 clusterView;column_major float4x4 clusterProjection;
    column_major float4x4 clusterInverseProjection;float4 clusterDepth;uint4 clusterDimensions;uint4 clusterCounts;};
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

#if !RENDERER_BINDLESS
Texture2D BaseColorTexture : register(t0, space0);
Texture2D MetallicRoughnessTexture : register(t4, space0);
Texture2D NormalTexture : register(t5, space0);
Texture2D OcclusionTexture : register(t6, space0);
Texture2D EmissiveTexture : register(t7, space0);
#endif
#if RENDERER_ENABLE_SHADOWS
#if RENDERER_BINDLESS
Texture2D ShadowMap : register(t28, space0);
#else
Texture2D ShadowMap : register(t2, space0);
#endif
#endif
SamplerState LinearSampler : register(s8, space0);
Texture2D<float4> ltcMatrixTexture:register(t26,space0);
Texture2D<float4> ltcAmplitudeTexture:register(t27,space0);
#if RENDERER_BINDLESS
Texture2D<float4> ScreenAOTexture : register(t22,space0);
Texture2D<float4> EnvironmentSpecularTexture : register(t23,space0);
Texture2D<float4> EnvironmentDiffuseTexture : register(t24,space0);
#else
Texture2D<float4> ScreenAOTexture : register(t16,space0);
Texture2D<float4> EnvironmentSpecularTexture : register(t13,space0);
Texture2D<float4> EnvironmentDiffuseTexture : register(t17,space0);
#endif
SamplerState ScreenSampler : register(s15,space0);
// Basic bindless rendering supplies neutral settings.x=0 and screen.z=0.
cbuffer EnvironmentConstants : register(b14,space0)
{
    float4 environmentSH[9];
    float4 environmentSettings;
    float4 environmentScreen;
    float4 environmentProbePosition;
    float4 environmentProbeBoxMin;
    float4 environmentProbeBoxMax;
};
float3 EnvironmentIrradiance(float3 n)
{
    float c=cos(environmentSettings.z),s=sin(environmentSettings.z);
    n.xy=float2(c*n.x+s*n.y,-s*n.x+c*n.y);
    float3 value=environmentSH[0].rgb*0.28209479177387814+
        environmentSH[1].rgb*(0.4886025119029199*n.y)+environmentSH[2].rgb*(0.4886025119029199*n.z)+
        environmentSH[3].rgb*(0.4886025119029199*n.x)+environmentSH[4].rgb*(1.0925484305920792*n.x*n.y)+
        environmentSH[5].rgb*(1.0925484305920792*n.y*n.z)+environmentSH[6].rgb*(0.31539156525252005*(3*n.z*n.z-1))+
        environmentSH[7].rgb*(1.0925484305920792*n.x*n.z)+environmentSH[8].rgb*(0.5462742152960396*(n.x*n.x-n.y*n.y));
    return max(value,0);
}
#if RENDERER_ENABLE_SHADOWS
SamplerState ShadowSampler : register(s9, space0);
#endif

struct PSInput
{
    float4 position           : SV_POSITION;
    float2 uv                 : TEXCOORD0;
    float3 worldPosition      : TEXCOORD1;
    float3 worldNormal        : TEXCOORD2;
    float4 worldTangent       : TEXCOORD3;
#if RENDERER_ENABLE_SHADOWS
    float4 lightSpacePosition : TEXCOORD4;
#endif
};

static const float PI = 3.14159265359;

float DistributionGGX(float3 normal, float3 halfway, float a2)
{
    float ndoth = clamp(dot(normal, halfway), 0.0, 1.0);
    float3 crossNH = cross(normal, halfway);
    // Avoid cancellation in 1 - (N.H)^2 near a narrow specular peak.
    float denom = dot(crossNH, crossNH) + ndoth * ndoth * a2;
    return a2 / (PI * denom * denom);
}

float GeometrySchlickGGX(float ndotv, float k)
{
    return ndotv / max(ndotv * (1.0 - k) + k, 0.0001);
}

float SchlickPow5(float base)
{
    float base2=base*base;
    return (base2*base2)*base;
}

float3 FresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0 - f0) * SchlickPow5(saturate(1.0 - cosTheta));
}

float3 FresnelSchlickRoughness(float cosTheta, float3 f0, float roughness)
{
    float3 roughnessF0 = max(float3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness), f0);
    return f0 + (roughnessF0 - f0) * SchlickPow5(saturate(1.0 - cosTheta));
}

#if RENDERER_BINDLESS
Texture2D materialTextures[19]:register(t0, space0);
StructuredBuffer<uint4> materialTextureIndices:register(t29, space0);
#endif
float4 SampleMaterial(uint kind,float2 uv)
{
#if RENDERER_BINDLESS
    uint index=materialTextureIndices[padding2*2u+kind/4u][kind%4u];
    return materialTextures[index].Sample(LinearSampler,uv);
#else
    if(kind==0u) return BaseColorTexture.Sample(LinearSampler,uv);
    if(kind==1u) return MetallicRoughnessTexture.Sample(LinearSampler,uv);
    if(kind==2u) return NormalTexture.Sample(LinearSampler,uv);
    if(kind==3u) return OcclusionTexture.Sample(LinearSampler,uv);
    return EmissiveTexture.Sample(LinearSampler,uv);
#endif
}

float3 GetNormal(PSInput input, uint textureFlags)
{
    float3 normal = normalize(input.worldNormal);
    float normalScale = materialParams.z;
    if ((textureFlags & RENDERER_TEXTURE_FLAG_NORMAL) == 0u || abs(normalScale) <= 0.0001)
    {
        return normal;
    }
    float3 tangent = normalize(input.worldTangent.xyz - normal * dot(normal, input.worldTangent.xyz));
    float3 bitangent = normalize(cross(normal, tangent)) * input.worldTangent.w;
    float3x3 tbn = float3x3(tangent, bitangent, normal);
    float3 tangentNormal = SampleMaterial(2u,input.uv).xyz * 2.0 - 1.0;
    tangentNormal.xy *= normalScale;
    if((padding1 & 8u)!=0u) tangentNormal.y=-tangentNormal.y;
    return normalize(mul(tangentNormal, tbn));
}

#if RENDERER_ENABLE_SHADOWS
struct ShadowData {
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
cbuffer ShadowMatrices:register(b3, space0) {ShadowData shadowMatrix;};

int2 DyShadowDimensions()
{
    uint width,height;ShadowMap.GetDimensions(width,height);return int2(width,height);
}
#define DY_SHADOW_PROJECTION(index) shadowMatrix.viewProjectionParams[index]
#define DY_SHADOW_MATRIX(index) shadowMatrix.lightViewProjectionMatrix[index]
#define DY_SHADOW_TILE(index) shadowMatrix.atlasRect[index]
#define DY_SHADOW_DIMENSIONS DyShadowDimensions()
#define DY_SHADOW_READ(pixel) ShadowMap.Load(int3(pixel,0)).r
#define DY_SHADOW_FILTER shadowMatrix.filterParams
#define DY_SHADOW_SOFT shadowMatrix.softShadowParams
#include "shadow_filter.hlsl"

float SampleShadowView(int index,float3 worldPosition,float3 normal,float3 lightDir,float strength,float sourceRadius,int pointBase)
{
    float4 clip=mul(shadowMatrix.lightViewProjectionMatrix[index],float4(worldPosition,1.0));
    if(clip.w<=0.0)return 1.0;
    float3 ndc=clip.xyz/clip.w;
    float2 uv=ndc.xy*.5+.5;
    uv.y=1.0-uv.y;
    if(uv.x<0.0||uv.y<0.0||uv.x>1.0||uv.y>1.0||ndc.z<0.0||ndc.z>1.0)return 1.0;
    float4 tile=shadowMatrix.atlasRect[index];
    uint width,height;ShadowMap.GetDimensions(width,height);float2 texel=1.0/float2(width,height);
    float2 minUv=tile.xy+texel*.5;
    float2 maxUv=tile.xy+tile.zw-texel*.5;
    float2 localUv=uv;
    uv=tile.xy+uv*tile.zw;
    float angle=1.0-max(dot(normal,lightDir),0.0);
    float bias=max(lighting.shadowParams.x,lighting.shadowParams.y*angle)+lighting.shadowParams.z*angle;
    float receiver=ndc.z-bias;
    if(sourceRadius>0 && shadowMatrix.softShadowParams.x>0)
        return 1-clamp(strength,0.0,1.0)*(1-DyShadowPcss(index,pointBase,localUv,receiver,clip.w,lightDir,sourceRadius));
    float radius=min(lighting.shadowParams.w,shadowMatrix.filterParams.z);
    int extent=int(ceil(radius));
    float visible=0.0,samples=0.0;
    for(int y=-extent;y<=extent;++y)for(int x=-extent;x<=extent;++x)
    {
        float2 sampleUv=clamp(uv+float2(x,y)*texel,minUv,maxUv);
        visible+=receiver<=ShadowMap.SampleLevel(ShadowSampler,sampleUv,0).r?1.0:0.0;
        samples+=1.0;
    }
    return 1.0-clamp(strength,0.0,1.0)*(1.0-visible/max(samples,1.0));
}

float SampleShadow(int type,int lightIndex,float3 worldPosition,float3 normal,float3 lightDir)
{
    if((drawTextureFlags & RENDERER_TEXTURE_FLAG_RECEIVE_SHADOW)==0u)return 1.0;
    float4 views=type==1?shadowMatrix.directionalViews[lightIndex]:(type==2?shadowMatrix.pointViews[lightIndex]:shadowMatrix.spotViews[lightIndex]);
    int count=int(views.y+.5);
    if(count==0)return 1.0;
    int local=0;
    float depth=0.0;
    if(type==1)
    {
        depth=-mul(shadowMatrix.cameraView,float4(worldPosition,1.0)).z;
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
    float visibility=SampleShadowView(index,worldPosition,normal,lightDir,views.z,views.w,type==2?int(views.x+.5):-1);
    if(type==1 && local<count-1)
    {
        float previous=local==0?shadowMatrix.filterParams.w:shadowMatrix.directionalSplits[lightIndex][local-1];
        float split=shadowMatrix.directionalSplits[lightIndex][local];
        float blend=smoothstep(lerp(previous,split,.9),split,depth);
        if(blend>0.0)visibility=lerp(visibility,SampleShadowView(index+1,worldPosition,normal,lightDir,views.z,views.w,type==2?int(views.x+.5):-1),blend);
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

struct DirectSurface
{
    float3 f0;
    float ndotv,alphaSquared,k,viewGeometry,diffuseFactor;
};
inline DirectSurface PrepareDirectSurface(float3 normal,float3 viewDir,float3 albedo,float metallic,float roughness)
{
    DirectSurface surface;
    surface.f0=lerp(float3(.04,.04,.04),albedo,metallic);
    surface.ndotv=max(dot(normal,viewDir),0.0);
    float a=roughness*roughness,r=roughness+1.0;
    surface.alphaSquared=a*a;
    surface.k=(r*r)/8.0;
    surface.viewGeometry=GeometrySchlickGGX(surface.ndotv,surface.k);
    surface.diffuseFactor=1.0-metallic;
    return surface;
}
inline float3 EvaluateDirectLight(
    float3 normal,
    float3 viewDir,
    float3 lightDir,
    float3 incidentIlluminance,
    float3 albedo,
    DirectSurface surface,
    float visibility) {
    float ndotl = max(dot(normal, lightDir), 0.0);
    float ndotv = surface.ndotv;
    if (ndotl <= 0.0 || ndotv <= 0.0 || visibility <= 0.0) {
        return float3(0.0,0.0,0.0);
    }
    float3 halfwayVector = viewDir + lightDir;
    float halfwayLengthSquared = dot(halfwayVector, halfwayVector);
    if (halfwayLengthSquared <= 0.000001) {
        return float3(0.0,0.0,0.0);
    }
    float3 halfway = halfwayVector * rsqrt(halfwayLengthSquared);
    float ndf = DistributionGGX(normal, halfway, surface.alphaSquared);
    float geometry = surface.viewGeometry * GeometrySchlickGGX(ndotl, surface.k);
    float3 fresnel = FresnelSchlick(max(dot(halfway, viewDir), 0.0), surface.f0);
    float3 specular = (ndf * geometry * fresnel) / max(4.0 * ndotv * ndotl, 0.0001);
    float3 diffuseWeight = (float3(1.0,1.0,1.0) - fresnel) * surface.diffuseFactor;
    return (diffuseWeight * albedo / PI + specular) * incidentIlluminance * ndotl * visibility;
}

#if RENDERER_ENABLE_AREA_LIGHTS
// LTC 적분과 표 배치는 Heitz 외 SIGGRAPH 2016 및 Assets/ltc/LICENSE를 따른다.
inline float LtcEdgeIntegral(float3 a,float3 b)
{
    float3 edge=cross(a,b);
    float sineSquared=dot(edge,edge),cosine=clamp(dot(a,b),-1.0,1.0);
    float limit=.25*cosine;
    if(cosine>=.5 && sineSquared<=limit*limit)
    {
        // The polynomial uses squared sine; avoid taking its square root just to square it again.
        float invCos=1.0/cosine,z=(sineSquared*invCos)*invCos;
        // atan(r)/r, 0<=r<=1/4: the omitted term is <=r^12/13<4.59e-9.
        float p=1.0+z*(-1.0/3.0+z*(1.0/5.0+z*(-1.0/7.0+z*(1.0/9.0-z/11.0))));
        float angleOverSine=p*invCos;
        if(sineSquared<=1e-24)
        {
            float sine=length(edge);
            if(sine<1e-12)angleOverSine*=sine/1e-12;
        }
        return edge.z*angleOverSine*0.15915494309189535;
    }
    float sine=length(edge);
    return edge.z*(atan2(sine,cosine)/max(sine,1e-12))*0.15915494309189535;
}
inline float3 LtcProject(float3 offset,float3 tangent,float3 bitangent,float3 normal)
{
    return float3(dot(offset,tangent),dot(offset,bitangent),dot(offset,normal));
}
inline float3 LtcTransform(float3 local,float4 inverseMatrix)
{
    return float3(inverseMatrix.x*local.x+inverseMatrix.z*local.z,local.y,
        inverseMatrix.y*local.x+inverseMatrix.w*local.z);
}
struct LtcSurface
{
    float3 normal,tangent,bitangent;
    float4 inverseMatrix;
    float3 specularWeight,diffuseWeight;
};
inline LtcSurface PrepareLtcSurface(float3 normal,float3 viewDir,DirectSurface directSurface,float roughness)
{
    LtcSurface surface;
    surface.normal=normal;
    surface.tangent=viewDir-normal*dot(normal,viewDir);
    if(dot(surface.tangent,surface.tangent)<1e-8)
        surface.tangent=cross(abs(normal.z)<.999?float3(0,0,1):float3(0,1,0),normal);
    surface.tangent=normalize(surface.tangent);
    surface.bitangent=cross(normal,surface.tangent);
    float ndotv=clamp(directSurface.ndotv,0.0,1.0);
    float2 coordinate=(float2(clamp(roughness,0.0,1.0),sqrt(1-ndotv))*63.0+.5)/64.0;
    surface.inverseMatrix=ltcMatrixTexture.SampleLevel(LinearSampler,coordinate,0);
    float4 amplitude=ltcAmplitudeTexture.SampleLevel(LinearSampler,coordinate,0);
    float3 f0=directSurface.f0;
    surface.specularWeight=f0*max(amplitude.x,0.0)+(float3(1,1,1)-f0)*max(amplitude.y,0.0);
    surface.diffuseWeight=(float3(1,1,1)-FresnelSchlickRoughness(ndotv,f0,roughness))*directSurface.diffuseFactor;
    return surface;
}
// Original float(corner)*float(pi/8) angles; retain their small axis asymmetries.
static const float2 LtcDiscDirections[16]={
    float2(1.0,0.0),float2(.9238795042,.3826834559),
    float2(.7071067691,.7071067691),float2(.3826834261,.9238795042),
    float2(-4.371138829e-8,1.0),float2(-.3826835155,.9238795042),
    float2(-.7071067691,.7071067691),float2(-.9238796234,.3826832771),
    float2(-1.0,-8.742277657e-8),float2(-.9238795042,-.3826834261),
    float2(-.7071066499,-.7071068883),float2(-.3826831281,-.9238796830),
    float2(1.192488064e-8,-1.0),float2(.3826836050,-.9238794446),
    float2(.7071070075,-.7071065307),float2(.9238795638,-.3826834261)};
inline float3 LtcPolygonPoint(float3 center,float3 axisX,float3 axisY,int corner,bool disc)
{
    float2 coordinate=disc?LtcDiscDirections[corner]:
        float2(corner==0 || corner==3?-1.0:1.0,corner<2?-1.0:1.0);
    return center+axisX*coordinate.x+axisY*coordinate.y;
}
inline void LtcAppendPoint(float3 clippedPoint,inout float3 first,inout float3 last,inout bool haveVertex,inout float sum)
{
    float squared=dot(clippedPoint,clippedPoint);
    if(squared<=1e-20)return;
    clippedPoint*=rsqrt(squared);
    if(haveVertex)sum+=LtcEdgeIntegral(last,clippedPoint);else {first=clippedPoint;haveVertex=true;}
    last=clippedPoint;
}
inline bool LtcPolygonAboveHorizon(float3 center,float3 axisX,float3 axisY,bool disc)
{
    float3 magnitude=abs(center)+abs(axisX)+abs(axisY);
    // Bound reconstructed corner lengths away from floating-point overflow.
    if(!all(isfinite(magnitude)) || any(magnitude>1e18))return false;
    float radius=disc?length(float2(axisX.z,axisY.z)):abs(axisX.z)+abs(axisY.z);
    float lower=center.z-radius;
    float margin=1e-5*max(1.0,abs(center.z)+abs(axisX.z)+abs(axisY.z));
    return isfinite(lower) && isfinite(margin) && lower>margin+1e-10;
}
inline float LtcPolygonIntegral(float4 inverseMatrix,float3 center,float3 axisX,float3 axisY,bool disc)
{
    float3 transformedCenter=LtcTransform(center,inverseMatrix);
    float3 transformedX=LtcTransform(axisX,inverseMatrix);
    float3 transformedY=LtcTransform(axisY,inverseMatrix);
    int count=disc?16:4;
    if(LtcPolygonAboveHorizon(transformedCenter,transformedX,transformedY,disc))
    {
        float3 firstFull=LtcPolygonPoint(transformedCenter,transformedX,transformedY,0,disc);
        firstFull*=rsqrt(dot(firstFull,firstFull));
        float3 previousFull=firstFull;float fullSum=0;
        for(int edge=1;edge<count;++edge)
        {
            float3 currentFull=LtcPolygonPoint(transformedCenter,transformedX,transformedY,edge,disc);
            currentFull*=rsqrt(dot(currentFull,currentFull));
            fullSum+=LtcEdgeIntegral(previousFull,currentFull);
            previousFull=currentFull;
        }
        fullSum+=LtcEdgeIntegral(previousFull,firstFull);
        return clamp(abs(fullSum),0.0,1.0);
    }
    float3 previous=LtcPolygonPoint(transformedCenter,transformedX,transformedY,count-1,disc);
    float3 first=float3(0,0,0),last=float3(0,0,0);bool haveVertex=false;float sum=0;
    for(int edge=0;edge<count;++edge)
    {
        float3 current=LtcPolygonPoint(transformedCenter,transformedX,transformedY,edge,disc);
        bool previousInside=previous.z>=0,currentInside=current.z>=0;
        // z=0 반구로 다각형을 자르되 정점 배열 대신 경계 순서대로 적분한다.
        if(previousInside!=currentInside)
        {
            float3 clippedPoint=lerp(previous,current,clamp(previous.z/(previous.z-current.z),0.0,1.0));
            clippedPoint.z=0;
            LtcAppendPoint(clippedPoint,first,last,haveVertex,sum);
        }
        if(currentInside)LtcAppendPoint(current,first,last,haveVertex,sum);
        previous=current;
    }
    if(haveVertex)sum+=LtcEdgeIntegral(last,first);
    return clamp(abs(sum),0.0,1.0);
}
inline bool LtcBelowHorizon(LtcSurface surface,float3 position,float3 center,float3 axisX,float3 axisY,
    float tangentCoefficient,float normalCoefficient,bool disc)
{
    float3 row=tangentCoefficient*surface.tangent+normalCoefficient*surface.normal;
    float centerZ=dot(center-position,row),x=dot(axisX,row),y=dot(axisY,row);
    float radius=disc?sqrt(x*x+y*y):abs(x)+abs(y);
    // Include uncancelled world coordinates and both basis terms in the rounding allowance.
    float3 magnitude=abs(center)+abs(position)+abs(axisX)+abs(axisY);
    float scale=abs(tangentCoefficient)*dot(magnitude,abs(surface.tangent))+
        abs(normalCoefficient)*dot(magnitude,abs(surface.normal));
    float bound=centerZ+radius,margin=1e-5*max(1.0,scale);
    return isfinite(bound) && isfinite(margin) && bound < -margin;
}
inline uint LtcIntegralMask(LtcSurface surface,float3 albedo,float3 position,float3 center,
    float3 axisX,float3 axisY,bool disc)
{
    uint mask=0u;
    if(any(surface.specularWeight!=0.0) &&
        !LtcBelowHorizon(surface,position,center,axisX,axisY,surface.inverseMatrix.y,surface.inverseMatrix.w,disc))
        mask|=1u;
    if(any(surface.diffuseWeight*albedo!=0.0) &&
        !LtcBelowHorizon(surface,position,center,axisX,axisY,0.0,1.0,disc))
        mask|=2u;
    return mask;
}
inline float3 EvaluateLtcPolygon(LtcSurface surface,float3 position,float3 center,float3 axisX,float3 axisY,
    bool disc,uint integralMask,float3 albedo,float3 radiance)
{
    // Both integrals share the surface-basis projection and retain their own inverse map.
    float3 localCenter=LtcProject(center-position,surface.tangent,surface.bitangent,surface.normal);
    float3 localX=LtcProject(axisX,surface.tangent,surface.bitangent,surface.normal);
    float3 localY=LtcProject(axisY,surface.tangent,surface.bitangent,surface.normal);
    float specular=0,diffuse=0;
    if((integralMask&1u)!=0u)specular=LtcPolygonIntegral(surface.inverseMatrix,localCenter,localX,localY,disc);
    if((integralMask&2u)!=0u)diffuse=LtcPolygonIntegral(float4(1,0,0,1),localCenter,localX,localY,disc);
    // 정규화된 cosine 적분이므로 diffuse에 1/pi나 거리 감쇠를 다시 곱하지 않는다.
    return radiance*(surface.specularWeight*specular+surface.diffuseWeight*albedo*diffuse);
}
inline float3 EvaluateRectAreaLight(RendererRectAreaLight light,LtcSurface surface,float3 albedo,float3 worldPosition)
{
    float3 direction=normalize(light.directionWidth.xyz);
    if(light.directionWidth.w<=0 || light.upHeight.w<=0 || dot(direction,worldPosition-light.positionIntensity.xyz)<=0)
        return float3(0,0,0);
    float3 right=normalize(cross(direction,light.upHeight.xyz));
    float3 up=normalize(cross(right,direction));
    float3 x=right*(light.directionWidth.w*.5),y=up*(light.upHeight.w*.5),center=light.positionIntensity.xyz;
    uint integralMask=LtcIntegralMask(surface,albedo,worldPosition,center,x,y,false);
    if(integralMask==0u)return float3(0,0,0);
    return EvaluateLtcPolygon(surface,worldPosition,center,x,y,false,integralMask,albedo,
        light.color.rgb*max(light.positionIntensity.w,0.0));
}
inline float3 EvaluateDiscAreaLight(RendererDiscAreaLight light,LtcSurface surface,float3 albedo,float3 worldPosition)
{
    float3 direction=normalize(light.directionRadius.xyz);
    if(light.directionRadius.w<=0 || dot(direction,worldPosition-light.positionIntensity.xyz)<=0)return float3(0,0,0);
    float3 right=normalize(cross(direction,light.up.xyz));
    float3 up=normalize(cross(right,direction));
    float3 x=right*light.directionRadius.w,y=up*light.directionRadius.w;
    uint integralMask=LtcIntegralMask(surface,albedo,worldPosition,light.positionIntensity.xyz,x,y,true);
    if(integralMask==0u)return float3(0,0,0);
    // Keep the original 16-sided disc boundary and stream it in LTC space.
    return EvaluateLtcPolygon(surface,worldPosition,light.positionIntensity.xyz,x,y,true,integralMask,albedo,
        light.color.rgb*max(light.positionIntensity.w,0.0));
}
#endif

inline float3 EvaluateLights(RendererLightingConstants lighting, float3 worldPosition, float3 normal, float3 viewDir,
    float3 albedo, float roughness,DirectSurface directSurface) {
    float3 directLight = float3(0.0,0.0,0.0);
    float ndotv=directSurface.ndotv;
    int directionalCount = clamp(int(lighting.lightCounts.x + 0.5), 0, RENDERER_MAX_DIRECTIONAL_LIGHTS);
    for (int lightIndex = 0; lightIndex < directionalCount; ++lightIndex) {
        RendererDirectionalLight light = lighting.directionalLights[lightIndex];
        float3 lightDir = normalize(light.directionIntensity.xyz);
        // Match EvaluateDirectLight's zero result before sampling its shadow.
        if(ndotv<=0.0 || max(dot(normal,lightDir),0.0)<=0.0)continue;
#if RENDERER_ENABLE_SHADOWS
        float visibility = SampleShadow(1,lightIndex,worldPosition,normal,lightDir);
#else
        float visibility = 1.0;
#endif
        directLight += EvaluateDirectLight(normal, viewDir, lightDir,
            light.color.rgb * max(light.directionIntensity.w, 0.0), albedo, directSurface, visibility);
    }
    uint base=ClusterBase(worldPosition);
    uint listed=0xffffffffu;
    if(base!=0xffffffffu)listed=clusterList[base];
    uint localCount=listed==0xffffffffu?clusterCounts.x:listed;
    for(uint item=0u;item<localCount;++item)
    {
        uint lightIndex=listed==0xffffffffu?item:clusterList[base+1u+item];
        ClusterLight light=clusterLights[lightIndex];
        float3 toLight=light.positionRange.xyz-worldPosition;
        float distanceToLight=length(toLight);
        if(distanceToLight<=0.0001 || distanceToLight>=light.positionRange.w)continue;
        float3 lightDir=toLight/distanceToLight;
        if(ndotv<=0.0 || max(dot(normal,lightDir),0.0)<=0.0)continue;
        float attenuation=PunctualAttenuation(distanceToLight,light.positionRange.w);
        int type=int(light.coneShadow.y+0.5),shadowIndex=int(light.coneShadow.z);
        if(type==3)attenuation*=SpotConeAttenuation(dot(-lightDir,light.directionOuterCos.xyz),
            light.coneShadow.x,light.directionOuterCos.w);
        if(attenuation<=0.0)continue;
        float3 incidentIlluminance=light.colorIntensity.rgb*max(light.colorIntensity.w,0.0)*attenuation;
#if RENDERER_ENABLE_SHADOWS
        float visibility=shadowIndex>=0?SampleShadow(type,shadowIndex,worldPosition,normal,lightDir):1.0;
#else
        float visibility=1.0;
#endif
        directLight+=EvaluateDirectLight(normal,viewDir,lightDir,incidentIlluminance,albedo,directSurface,visibility);
    }
#if RENDERER_ENABLE_AREA_LIGHTS
    int rectAreaCount = clamp(int(lighting.areaLightCounts.x + 0.5), 0, RENDERER_MAX_RECT_AREA_LIGHTS);
    int discAreaCount = clamp(int(lighting.areaLightCounts.y + 0.5), 0, RENDERER_MAX_DISC_AREA_LIGHTS);
    if(ndotv>0.0 && rectAreaCount+discAreaCount>0) {
        // Prepare once; both area-light loops consume the same read-only surface.
        LtcSurface surface=PrepareLtcSurface(normal,viewDir,directSurface,roughness);
        for (int lightIndex = 0; lightIndex < rectAreaCount; ++lightIndex)
            directLight += EvaluateRectAreaLight(lighting.rectAreaLights[lightIndex],surface,albedo,worldPosition);
        for (int lightIndex = 0; lightIndex < discAreaCount; ++lightIndex)
            directLight += EvaluateDiscAreaLight(lighting.discAreaLights[lightIndex],surface,albedo,worldPosition);
    }
#endif

    return directLight;
}

#if RENDERER_DEPTH_PREPASS
[earlydepthstencil]
#endif
float4 main(PSInput input, bool frontFace : SV_IsFrontFace) : SV_TARGET
{
    uint textureFlags = drawTextureFlags;
    float4 base = baseColor;
    if ((textureFlags & RENDERER_TEXTURE_FLAG_BASE_COLOR) != 0u)
    {
        base *= SampleMaterial(0u,input.uv);
    }
    uint alphaMode=padding1 & 3u;
    if((textureFlags&64u)!=0u)
    {
        if(alphaMode==1u) clip(base.a-asfloat(padding0));
        float3 color=baseColor.rgb;
        if(lighting.pbrParams.z>.5) color=pow(color,1.0/2.2);
        return float4(color,alphaMode==2u ? base.a : 1.0);
    }
    if((padding1&16u)!=0u)
    {
        if(alphaMode==1u)clip(base.a-asfloat(padding0));
        float3 color=base.rgb;
        if(lighting.pbrParams.z>=0.0)
        {
            color=min(color*lighting.pbrParams.w,16777216.0);
            color=color/(color+1.0);
            if(lighting.pbrParams.z>.5)color=pow(color,1.0/2.2);
        }
        color=float3(isnan(color.r)?0:clamp(color.r,0,65504),isnan(color.g)?0:clamp(color.g,0,65504),
            isnan(color.b)?0:clamp(color.b,0,65504));
        return float4(color,alphaMode==2u?base.a:1.0);
    }
    float3 albedo=base.rgb;

    float minRoughness = clamp(lighting.pbrParams.x, 0.01, 1.0);
    float ambientSpecularStrength = max(lighting.pbrParams.y, 0.0);
    float metallic = clamp(materialParams.x, 0.0, 1.0);
    float roughness = clamp(materialParams.y, minRoughness, 1.0);
    float occlusion = 1.0;
    // Opaque environment buffers already include authored AO and screen AO.
    bool usesEnvironmentBuffers=environmentSettings.x>0.5 && alphaMode!=2u;
    if ((textureFlags & RENDERER_TEXTURE_FLAG_METALLIC_ROUGHNESS) != 0u)
    {
        float4 metallicRoughness = SampleMaterial(1u,input.uv);
        roughness = clamp(roughness * metallicRoughness.g, minRoughness, 1.0);
        metallic = clamp(metallic * metallicRoughness.b, 0.0, 1.0);
    }
    if ((textureFlags & RENDERER_TEXTURE_FLAG_OCCLUSION) != 0u && !usesEnvironmentBuffers)
    {
        occlusion = lerp(1.0, SampleMaterial(3u,input.uv).r, clamp(materialParams.w, 0.0, 1.0));
    }
    float3 emissive = emissiveColor.rgb;
    if ((textureFlags & RENDERER_TEXTURE_FLAG_EMISSIVE) != 0u)
    {
        emissive *= SampleMaterial(4u,input.uv).rgb;
    }

    float3 normal = GetNormal(input, textureFlags);
    // Derivatives precede discard and the two-sided normal orientation branch.
    float3 normalDx=ddx(normal),normalDy=ddy(normal);
    float normalVariance=0.25*(dot(normalDx,normalDx)+dot(normalDy,normalDy));
    float filteredAlphaSquared=min(1.0,roughness*roughness*roughness*roughness+min(2.0*normalVariance,0.18));
    roughness=sqrt(sqrt(filteredAlphaSquared));
    if(alphaMode==1u) clip(base.a-asfloat(padding0));
    if((padding1 & 4u)!=0u && !frontFace) normal=-normal;
    float3 viewDir = normalize(lighting.cameraPosition.xyz - input.worldPosition);
    DirectSurface directSurface=PrepareDirectSurface(normal,viewDir,albedo,metallic,roughness);
    float3 f0=directSurface.f0;
    float3 directLight = EvaluateLights(lighting, input.worldPosition, normal, viewDir, albedo, roughness,directSurface);
    float3 ambientFresnel = FresnelSchlickRoughness(directSurface.ndotv, f0, roughness);
    float3 kD = (float3(1.0, 1.0, 1.0) - ambientFresnel) * (1.0 - metallic);
    float3 ambientDiffuse = kD * albedo * lighting.ambientColor.rgb * lighting.ambientColor.a;
    float3 ambientSpecular = ambientFresnel * lighting.environmentColor.rgb * lighting.environmentColor.a * ambientSpecularStrength;
    float3 ambient = (ambientDiffuse + ambientSpecular) * occlusion;
    float2 screenUV=input.position.xy*environmentScreen.xy;
    float screenAO=!usesEnvironmentBuffers && environmentScreen.z>0.5 ? ScreenAOTexture.SampleLevel(ScreenSampler,screenUV,0).r : 1.0;
    ambient*=screenAO;
    if(environmentSettings.x>0.5)
    {
        ambient=0;
        if(alphaMode==2u)
        {
            if((uint(environmentScreen.w+0.5)&1u)!=0u)
                ambient=kD*albedo*EnvironmentIrradiance(normal)*(0.3183098861837907*environmentSettings.y*occlusion*screenAO);
        }
        else
        {
            ambient=kD*albedo*EnvironmentDiffuseTexture.SampleLevel(ScreenSampler,screenUV,0).rgb;
            ambient+=EnvironmentSpecularTexture.SampleLevel(ScreenSampler,screenUV,0).rgb;
        }
    }
    float3 color = lighting.lightCounts.w > 0.5 ? ambient + directLight + emissive : albedo + emissive;

    if(lighting.pbrParams.z >= 0.0) {
        color = min(color * lighting.pbrParams.w, 16777216.0);
    color = color / (color + float3(1.0, 1.0, 1.0));
    if (lighting.pbrParams.z > 0.5) color = pow(color, float3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2));
    }
    // Preserve finite RGBA16F scene values before screen-space/temporal filtering.
    color=float3(isnan(color.r)?0.0:clamp(color.r,0.0,65504.0),
        isnan(color.g)?0.0:clamp(color.g,0.0,65504.0),isnan(color.b)?0.0:clamp(color.b,0.0,65504.0));
    return float4(color, alphaMode==2u ? base.a : 1.0);
}
