#version 450
#extension GL_GOOGLE_include_directive : require

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
#if RENDERER_DEPTH_PREPASS
layout(early_fragment_tests) in;
#endif
#define RENDERER_DESCRIPTOR_SET 0
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

#define RENDERER_TEXTURE_FLAG_BASE_COLOR 1
#define RENDERER_TEXTURE_FLAG_METALLIC_ROUGHNESS 2
#define RENDERER_TEXTURE_FLAG_NORMAL 4
#define RENDERER_TEXTURE_FLAG_OCCLUSION 8
#define RENDERER_TEXTURE_FLAG_EMISSIVE 16
#define RENDERER_TEXTURE_FLAG_RECEIVE_SHADOW 32

#ifndef RENDERER_ENABLE_SHADOWS
#error RENDERER_ENABLE_SHADOWS must be defined
#endif

layout(location = 0) in vec2 fragUv;
layout(location = 1) in vec3 fragWorldPosition;
layout(location = 2) in vec3 fragNormal;
layout(location = 3) in vec4 fragTangent;
layout(location = 5) in vec4 fragColor;
#if RENDERER_ENABLE_SHADOWS
layout(location = 4) in vec4 fragLightSpacePosition;
#endif
layout(location = 0) out vec4 outColor;

#if !RENDERER_BINDLESS
layout(set = RENDERER_DESCRIPTOR_SET, binding = RENDERER_BINDING_BASE_COLOR_TEXTURE) uniform texture2D baseColorTexture;
#endif
#if RENDERER_ENABLE_SHADOWS
layout(set = RENDERER_DESCRIPTOR_SET, binding = RENDERER_BINDING_SHADOW_TEXTURE) uniform texture2D shadowMap;
#endif
#if !RENDERER_BINDLESS
layout(set = RENDERER_DESCRIPTOR_SET, binding = RENDERER_BINDING_METALLIC_ROUGHNESS_TEXTURE) uniform texture2D metallicRoughnessTexture;
layout(set = RENDERER_DESCRIPTOR_SET, binding = RENDERER_BINDING_NORMAL_TEXTURE) uniform texture2D normalTexture;
layout(set = RENDERER_DESCRIPTOR_SET, binding = RENDERER_BINDING_OCCLUSION_TEXTURE) uniform texture2D occlusionTexture;
layout(set = RENDERER_DESCRIPTOR_SET, binding = RENDERER_BINDING_EMISSIVE_TEXTURE) uniform texture2D emissiveTexture;
#endif
layout(set = RENDERER_DESCRIPTOR_SET, binding = RENDERER_BINDING_MATERIAL_SAMPLER) uniform sampler materialSampler;
layout(set=0,binding=26) uniform texture2D ltcMatrixTexture;
layout(set=0,binding=27) uniform texture2D ltcAmplitudeTexture;
#if RENDERER_BINDLESS
layout(set=0,binding=22) uniform texture2D screenAOTexture;
layout(set=0,binding=23) uniform texture2D environmentSpecularTexture;
layout(set=0,binding=24) uniform texture2D environmentDiffuseTexture;
#else
layout(set=0,binding=16) uniform texture2D screenAOTexture;
layout(set=0,binding=13) uniform texture2D environmentSpecularTexture;
layout(set=0,binding=17) uniform texture2D environmentDiffuseTexture;
#endif
layout(set=0,binding=15) uniform sampler screenSampler;
// Basic bindless rendering supplies neutral settings.x=0 and screen.z=0.
layout(set=0,binding=14) uniform EnvironmentConstants
{
    vec4 sh[9];
    vec4 settings;
    vec4 screen;
    vec4 probePosition;
    vec4 probeBoxMin;
    vec4 probeBoxMax;
} environment;
vec3 EnvironmentIrradiance(vec3 n)
{
    float c=cos(environment.settings.z),s=sin(environment.settings.z);
    n.xy=mat2(c,-s,s,c)*n.xy;
    vec3 value=environment.sh[0].rgb*0.28209479177387814+
        environment.sh[1].rgb*(0.4886025119029199*n.y)+environment.sh[2].rgb*(0.4886025119029199*n.z)+
        environment.sh[3].rgb*(0.4886025119029199*n.x)+environment.sh[4].rgb*(1.0925484305920792*n.x*n.y)+
        environment.sh[5].rgb*(1.0925484305920792*n.y*n.z)+environment.sh[6].rgb*(0.31539156525252005*(3*n.z*n.z-1))+
        environment.sh[7].rgb*(1.0925484305920792*n.x*n.z)+environment.sh[8].rgb*(0.5462742152960396*(n.x*n.x-n.y*n.y));
    return max(value,vec3(0));
}
#if RENDERER_ENABLE_SHADOWS
layout(set = RENDERER_DESCRIPTOR_SET, binding = RENDERER_BINDING_SHADOW_SAMPLER) uniform sampler shadowSampler;
#endif

// CPU가 업로드하는 조명 버퍼의 필드 순서와 크기를 유지한다.
struct RendererDirectionalLight
{
	vec4 directionIntensity;
	vec4 color;
};

struct RendererPointLight
{
	vec4 positionRange;
	vec4 colorIntensity;
};

struct RendererSpotLight
{
	vec4 positionRange;
	vec4 directionOuterCos;
	vec4 colorIntensity;
	vec4 coneParams;
};

struct RendererRectAreaLight
{
	vec4 positionIntensity;
	vec4 directionWidth;
	vec4 upHeight;
	vec4 color;
};

struct RendererDiscAreaLight
{
	vec4 positionIntensity;
	vec4 directionRadius;
	vec4 up;
	vec4 color;
};

struct RendererLightingConstants
{
	vec4 cameraPosition;
	vec4 directionalLightDirection;
	vec4 directionalLightColor;
	vec4 ambientColor;
	vec4 shadowParams;
	vec4 pbrParams;
	vec4 environmentColor;
	vec4 pointLightPositionRange;
	vec4 pointLightColorIntensity;
	vec4 lightCounts;
	vec4 areaLightCounts;
	RendererDirectionalLight directionalLights[RENDERER_MAX_DIRECTIONAL_LIGHTS];
	RendererPointLight pointLights[RENDERER_MAX_POINT_LIGHTS];
	RendererSpotLight spotLights[RENDERER_MAX_SPOT_LIGHTS];
	RendererRectAreaLight rectAreaLights[RENDERER_MAX_RECT_AREA_LIGHTS];
	RendererDiscAreaLight discAreaLights[RENDERER_MAX_DISC_AREA_LIGHTS];
	vec4 shadowLight;
};
layout(set = RENDERER_DESCRIPTOR_SET, binding = RENDERER_BINDING_LIGHTING_CONSTANTS) uniform RendererLighting { RendererLightingConstants lighting; };
struct ClusterLight {vec4 positionRange;vec4 colorIntensity;vec4 directionOuterCos;vec4 coneShadow;};
layout(std430,set=0,binding=19) readonly buffer ClusterLights {ClusterLight clusterLights[];};
layout(std430,set=0,binding=20) readonly buffer ClusterLists {uint clusterList[];};
layout(set=0,binding=21) uniform ClusterParameters {mat4 view;mat4 projection;mat4 inverseProjection;vec4 depth;uvec4 dimensions;uvec4 counts;} clusters;
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

layout(push_constant) uniform DrawConstants {
    mat4 viewProjectionMatrix;
    mat4 modelMatrix;
    uint textureFlags;
    uint padding0;
    uint padding1;
    uint padding2;
    vec4 emissiveColor;
    vec4 baseColor;
    vec4 materialParams;
} pushConstants;

const float PI = 3.14159265359;

float DistributionGGX(vec3 normal, vec3 halfway, float a2) {
    float ndoth = clamp(dot(normal, halfway), 0.0, 1.0);
    vec3 crossNH = cross(normal, halfway);
    // Avoid cancellation in 1 - (N.H)^2 near a narrow specular peak.
    float denom = dot(crossNH, crossNH) + ndoth * ndoth * a2;
    return a2 / (PI * denom * denom);
}

float GeometrySchlickGGX(float ndotv, float k) {
    return ndotv / max(ndotv * (1.0 - k) + k, 0.0001);
}

float SchlickPow5(float base) {
    float base2=base*base;
    return (base2*base2)*base;
}

vec3 FresnelSchlick(float cosTheta, vec3 f0) {
    return f0 + (1.0 - f0) * SchlickPow5(clamp(1.0 - cosTheta, 0.0, 1.0));
}

vec3 FresnelSchlickRoughness(float cosTheta, vec3 f0, float roughness) {
    vec3 roughnessF0 = max(vec3(1.0 - roughness), f0);
    return f0 + (roughnessF0 - f0) * SchlickPow5(clamp(1.0 - cosTheta, 0.0, 1.0));
}

#if RENDERER_BINDLESS
layout(set=0,binding=0) uniform texture2D materialTextures[19];
layout(std430,set=0,binding=29) readonly buffer MaterialTextureIndices {uvec4 materialTextureIndices[];};
#endif
vec4 SampleMaterial(uint kind,vec2 uv)
{
#if RENDERER_BINDLESS
    uint index=materialTextureIndices[pushConstants.padding2*2u+kind/4u][kind%4u];
    return texture(sampler2D(materialTextures[index],materialSampler),uv);
#else
    if(kind==0u) return texture(sampler2D(baseColorTexture,materialSampler),uv);
    if(kind==1u) return texture(sampler2D(metallicRoughnessTexture,materialSampler),uv);
    if(kind==2u) return texture(sampler2D(normalTexture,materialSampler),uv);
    if(kind==3u) return texture(sampler2D(occlusionTexture,materialSampler),uv);
    return texture(sampler2D(emissiveTexture,materialSampler),uv);
#endif
}

vec3 GetNormal() {
    vec3 normal = normalize(fragNormal);
    int textureFlags = int(pushConstants.textureFlags);
    float normalScale = pushConstants.materialParams.z;
    if ((textureFlags & RENDERER_TEXTURE_FLAG_NORMAL) == 0 || abs(normalScale) <= 0.0001) {
        return normal;
    }

    vec3 tangent = normalize(fragTangent.xyz - normal * dot(normal, fragTangent.xyz));
    vec3 bitangent = normalize(cross(normal, tangent)) * fragTangent.w;
    mat3 tbn = mat3(tangent, bitangent, normal);
    vec3 tangentNormal = SampleMaterial(2u,fragUv).xyz * 2.0 - 1.0;
    tangentNormal.xy *= normalScale;
    if((pushConstants.padding1 & 8u)!=0u) tangentNormal.y=-tangentNormal.y;
    return normalize(tbn * tangentNormal);
}

#if RENDERER_ENABLE_SHADOWS
layout(set=0,binding=RENDERER_BINDING_SHADOW_MATRIX) uniform ShadowMatrix {
    mat4 lightViewProjectionMatrix[128];
    vec4 atlasRect[128];
    vec4 directionalViews[RENDERER_MAX_DIRECTIONAL_LIGHTS];
    vec4 pointViews[RENDERER_MAX_POINT_LIGHTS];
    vec4 spotViews[RENDERER_MAX_SPOT_LIGHTS];
    vec4 directionalSplits[RENDERER_MAX_DIRECTIONAL_LIGHTS];
    mat4 cameraView;
    vec4 filterParams;
    vec4 viewProjectionParams[128];
    vec4 softShadowParams;
} shadowMatrix;

#define DY_SHADOW_PROJECTION(index) shadowMatrix.viewProjectionParams[index]
#define DY_SHADOW_MATRIX(index) shadowMatrix.lightViewProjectionMatrix[index]
#define DY_SHADOW_TILE(index) shadowMatrix.atlasRect[index]
#define DY_SHADOW_DIMENSIONS textureSize(sampler2D(shadowMap,shadowSampler),0)
#define DY_SHADOW_READ(pixel) texelFetch(sampler2D(shadowMap,shadowSampler),pixel,0).r
#define DY_SHADOW_FILTER shadowMatrix.filterParams
#define DY_SHADOW_SOFT shadowMatrix.softShadowParams
#include "shadow_filter.glsl"

float SampleShadowView(int index,vec3 worldPosition,vec3 normal,vec3 lightDir,float strength,float sourceRadius,int pointBase)
{
    vec4 clip=shadowMatrix.lightViewProjectionMatrix[index]*vec4(worldPosition,1.0);
    if(clip.w<=0.0)return 1.0;
    vec3 ndc=clip.xyz/clip.w;
    vec2 uv=ndc.xy*.5+.5;
    uv.y=1.0-uv.y;
    if(uv.x<0.0||uv.y<0.0||uv.x>1.0||uv.y>1.0||ndc.z<0.0||ndc.z>1.0)return 1.0;
    vec4 tile=shadowMatrix.atlasRect[index];
    vec2 texel=1.0/vec2(textureSize(sampler2D(shadowMap,shadowSampler),0));
    vec2 minUv=tile.xy+texel*.5;
    vec2 maxUv=tile.xy+tile.zw-texel*.5;
    vec2 localUv=uv;
    uv=tile.xy+uv*tile.zw;
    float angle=1.0-max(dot(normal,lightDir),0.0);
    float bias=max(lighting.shadowParams.x,lighting.shadowParams.y*angle)+lighting.shadowParams.z*angle;
    float receiver=ndc.z-bias;
    if(sourceRadius>0 && shadowMatrix.softShadowParams.x>0)
        return 1-clamp(strength,0.0,1.0)*(1-DyShadowPcss(index,pointBase,localUv,receiver,clip.w,lightDir,sourceRadius));
    float radius=min(lighting.shadowParams.w,shadowMatrix.filterParams.z);
    int extent=int(ceil(radius));
    float visible=0.0,samples=float((2*extent+1)*(2*extent+1));
    for(int y=-extent;y<=extent;++y)for(int x=-extent;x<=extent;++x)
    {
        vec2 sampleUv=clamp(uv+vec2(x,y)*texel,minUv,maxUv);
        visible+=receiver<=textureLod(sampler2D(shadowMap,shadowSampler),sampleUv,0).r?1.0:0.0;
    }
    return 1.0-clamp(strength,0.0,1.0)*(1.0-visible/max(samples,1.0));
}

float SampleShadow(int type,int lightIndex,vec3 worldPosition,vec3 normal,vec3 lightDir)
{
    if((pushConstants.textureFlags & RENDERER_TEXTURE_FLAG_RECEIVE_SHADOW)==0u)return 1.0;
    vec4 views=type==1?shadowMatrix.directionalViews[lightIndex]:(type==2?shadowMatrix.pointViews[lightIndex]:shadowMatrix.spotViews[lightIndex]);
    int count=int(views.y+.5);
    if(count==0)return 1.0;
    int local=0;
    float depth=0.0;
    if(type==1)
    {
        depth=-(shadowMatrix.cameraView*vec4(worldPosition,1.0)).z;
        float finalSplit=shadowMatrix.directionalSplits[lightIndex][count-1];
        if(finalSplit>0 && depth>finalSplit)return 1.0;
        while(local<count-1 && depth>shadowMatrix.directionalSplits[lightIndex][local])++local;
    }
    if(type==2)
    {
        vec3 direction=-lightDir, magnitude=abs(direction);
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
        float blend=smoothstep(mix(previous,split,.9),split,depth);
        if(blend>0.0)visibility=mix(visibility,SampleShadowView(index+1,worldPosition,normal,lightDir,views.z,views.w,type==2?int(views.x+.5):-1),blend);
    }
    return visibility;
}

#endif

// 현재 언어의 조명 계산을 직접 작성한다.
float PunctualAttenuation(float distanceToLight, float range) {
    if (range <= 0.0 || distanceToLight >= range) {
        return 0.0;
    }
    float distanceSquared = max(distanceToLight * distanceToLight, 0.0001);
    float ratio = distanceToLight / range;
    float ratioSquared = ratio * ratio;
    float window = max(1.0 - ratioSquared * ratioSquared, 0.0);
    return (window * window) / distanceSquared;
}

float SpotConeAttenuation(float cosTheta, float innerCos, float outerCos) {
    float denominator = max(innerCos - outerCos, 0.0001);
    float t = clamp((cosTheta - outerCos) / denominator, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

struct DirectSurface
{
    vec3 f0;
    float ndotv,alphaSquared,k,viewGeometry,diffuseFactor;
};
DirectSurface PrepareDirectSurface(vec3 normal,vec3 viewDir,vec3 albedo,float metallic,float roughness)
{
    DirectSurface surface;
    surface.f0=mix(vec3(.04),albedo,metallic);
    surface.ndotv=max(dot(normal,viewDir),0.0);
    float a=roughness*roughness,r=roughness+1.0;
    surface.alphaSquared=a*a;
    surface.k=(r*r)/8.0;
    surface.viewGeometry=GeometrySchlickGGX(surface.ndotv,surface.k);
    surface.diffuseFactor=1.0-metallic;
    return surface;
}
vec3 EvaluateDirectLight(
    vec3 normal,
    vec3 viewDir,
    vec3 lightDir,
    vec3 incidentIlluminance,
    vec3 albedo,
    DirectSurface surface,
    float visibility) {
    float ndotl = max(dot(normal, lightDir), 0.0);
    float ndotv = surface.ndotv;
    if (ndotl <= 0.0 || ndotv <= 0.0 || visibility <= 0.0) {
        return vec3(0.0,0.0,0.0);
    }
    vec3 halfwayVector = viewDir + lightDir;
    float halfwayLengthSquared = dot(halfwayVector, halfwayVector);
    if (halfwayLengthSquared <= 0.000001) {
        return vec3(0.0,0.0,0.0);
    }
    vec3 halfway = halfwayVector * inversesqrt(halfwayLengthSquared);
    float ndf = DistributionGGX(normal, halfway, surface.alphaSquared);
    float geometry = surface.viewGeometry * GeometrySchlickGGX(ndotl, surface.k);
    vec3 fresnel = FresnelSchlick(max(dot(halfway, viewDir), 0.0), surface.f0);
    vec3 specular = (ndf * geometry * fresnel) / max(4.0 * ndotv * ndotl, 0.0001);
    vec3 diffuseWeight = (vec3(1.0,1.0,1.0) - fresnel) * surface.diffuseFactor;
    return (diffuseWeight * albedo / PI + specular) * incidentIlluminance * ndotl * visibility;
}

#if RENDERER_ENABLE_AREA_LIGHTS
// LTC 적분과 표 배치는 Heitz 외 SIGGRAPH 2016 및 Assets/ltc/LICENSE를 따른다.
float LtcEdgeIntegral(vec3 a,vec3 b)
{
    vec3 edge=cross(a,b);
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
    return edge.z*(atan(sine,cosine)/max(sine,1e-12))*0.15915494309189535;
}
vec3 LtcProject(vec3 offset,vec3 tangent,vec3 bitangent,vec3 normal)
{
    return vec3(dot(offset,tangent),dot(offset,bitangent),dot(offset,normal));
}
vec3 LtcTransform(vec3 local,vec4 inverseMatrix)
{
    return vec3(inverseMatrix.x*local.x+inverseMatrix.z*local.z,local.y,
        inverseMatrix.y*local.x+inverseMatrix.w*local.z);
}
struct LtcSurface
{
    vec3 normal,tangent,bitangent;
    vec4 inverseMatrix;
    vec3 specularWeight,diffuseWeight;
};
LtcSurface PrepareLtcSurface(vec3 normal,vec3 viewDir,DirectSurface directSurface,float roughness)
{
    LtcSurface surface;
    surface.normal=normal;
    surface.tangent=viewDir-normal*dot(normal,viewDir);
    if(dot(surface.tangent,surface.tangent)<1e-8)
        surface.tangent=cross(abs(normal.z)<.999?vec3(0,0,1):vec3(0,1,0),normal);
    surface.tangent=normalize(surface.tangent);
    surface.bitangent=cross(normal,surface.tangent);
    float ndotv=clamp(directSurface.ndotv,0.0,1.0);
    vec2 coordinate=(vec2(clamp(roughness,0.0,1.0),sqrt(1-ndotv))*63.0+.5)/64.0;
    surface.inverseMatrix=textureLod(sampler2D(ltcMatrixTexture,materialSampler),coordinate,0);
    vec4 amplitude=textureLod(sampler2D(ltcAmplitudeTexture,materialSampler),coordinate,0);
    vec3 f0=directSurface.f0;
    surface.specularWeight=f0*max(amplitude.x,0.0)+(vec3(1)-f0)*max(amplitude.y,0.0);
    surface.diffuseWeight=(vec3(1)-FresnelSchlickRoughness(ndotv,f0,roughness))*directSurface.diffuseFactor;
    return surface;
}
// cos/sin of the original float(corner)*float(pi/8) angles, rounded to float.
// Keep their small axis asymmetries instead of replacing the original polygon.
const vec2 LtcDiscDirections[16]=vec2[16](
    vec2(1.0,0.0),vec2(.9238795042,.3826834559),
    vec2(.7071067691,.7071067691),vec2(.3826834261,.9238795042),
    vec2(-4.371138829e-8,1.0),vec2(-.3826835155,.9238795042),
    vec2(-.7071067691,.7071067691),vec2(-.9238796234,.3826832771),
    vec2(-1.0,-8.742277657e-8),vec2(-.9238795042,-.3826834261),
    vec2(-.7071066499,-.7071068883),vec2(-.3826831281,-.9238796830),
    vec2(1.192488064e-8,-1.0),vec2(.3826836050,-.9238794446),
    vec2(.7071070075,-.7071065307),vec2(.9238795638,-.3826834261));
vec3 LtcPolygonPoint(vec3 center,vec3 axisX,vec3 axisY,int corner,bool disc)
{
    vec2 coordinate=disc?LtcDiscDirections[corner]:
        vec2(corner==0 || corner==3?-1.0:1.0,corner<2?-1.0:1.0);
    return center+axisX*coordinate.x+axisY*coordinate.y;
}
void LtcAppendPoint(vec3 clippedPoint,inout vec3 first,inout vec3 last,inout bool haveVertex,inout float sum)
{
    float squared=dot(clippedPoint,clippedPoint);
    if(squared<=1e-20)return;
    clippedPoint*=inversesqrt(squared);
    if(haveVertex)sum+=LtcEdgeIntegral(last,clippedPoint);else {first=clippedPoint;haveVertex=true;}
    last=clippedPoint;
}
bool LtcPolygonAboveHorizon(vec3 center,vec3 axisX,vec3 axisY,bool disc)
{
    vec3 magnitude=abs(center)+abs(axisX)+abs(axisY);
    // This also keeps every corner's squared length finite after reconstruction.
    if(any(isnan(magnitude)) || any(isinf(magnitude)) || any(greaterThan(magnitude,vec3(1e18))))return false;
    float radius=disc?length(vec2(axisX.z,axisY.z)):abs(axisX.z)+abs(axisY.z);
    float lower=center.z-radius;
    // Covers the disc table's float rounding and reconstructed corner additions.
    float margin=1e-5*max(1.0,abs(center.z)+abs(axisX.z)+abs(axisY.z));
    return !isnan(lower) && !isinf(lower) && !isnan(margin) && !isinf(margin) && lower>margin+1e-10;
}
float LtcPolygonIntegral(vec4 inverseMatrix,vec3 center,vec3 axisX,vec3 axisY,bool disc)
{
    vec3 transformedCenter=LtcTransform(center,inverseMatrix);
    vec3 transformedX=LtcTransform(axisX,inverseMatrix);
    vec3 transformedY=LtcTransform(axisY,inverseMatrix);
    int count=disc?16:4;
    if(LtcPolygonAboveHorizon(transformedCenter,transformedX,transformedY,disc))
    {
        vec3 firstFull=LtcPolygonPoint(transformedCenter,transformedX,transformedY,0,disc);
        firstFull*=inversesqrt(dot(firstFull,firstFull));
        vec3 previousFull=firstFull;float fullSum=0;
        for(int edge=1;edge<count;++edge)
        {
            vec3 currentFull=LtcPolygonPoint(transformedCenter,transformedX,transformedY,edge,disc);
            currentFull*=inversesqrt(dot(currentFull,currentFull));
            fullSum+=LtcEdgeIntegral(previousFull,currentFull);
            previousFull=currentFull;
        }
        fullSum+=LtcEdgeIntegral(previousFull,firstFull);
        return clamp(abs(fullSum),0.0,1.0);
    }
    vec3 previous=LtcPolygonPoint(transformedCenter,transformedX,transformedY,count-1,disc);
    vec3 first=vec3(0),last=vec3(0);bool haveVertex=false;float sum=0;
    for(int edge=0;edge<count;++edge)
    {
        vec3 current=LtcPolygonPoint(transformedCenter,transformedX,transformedY,edge,disc);
        bool previousInside=previous.z>=0,currentInside=current.z>=0;
        // z=0 반구로 다각형을 자르되 정점 배열 대신 경계 순서대로 적분한다.
        if(previousInside!=currentInside)
        {
            vec3 clippedPoint=mix(previous,current,clamp(previous.z/(previous.z-current.z),0.0,1.0));
            clippedPoint.z=0;
            LtcAppendPoint(clippedPoint,first,last,haveVertex,sum);
        }
        if(currentInside)LtcAppendPoint(current,first,last,haveVertex,sum);
        previous=current;
    }
    if(haveVertex)sum+=LtcEdgeIntegral(last,first);
    return clamp(abs(sum),0.0,1.0);
}
bool LtcBelowHorizon(LtcSurface surface,vec3 position,vec3 center,vec3 axisX,vec3 axisY,
    float tangentCoefficient,float normalCoefficient,bool disc)
{
    vec3 row=tangentCoefficient*surface.tangent+normalCoefficient*surface.normal;
    float centerZ=dot(center-position,row),x=dot(axisX,row),y=dot(axisY,row);
    float radius=disc?sqrt(x*x+y*y):abs(x)+abs(y);
    // Include uncancelled world coordinates and both basis terms in the rounding allowance.
    vec3 magnitude=abs(center)+abs(position)+abs(axisX)+abs(axisY);
    float scale=abs(tangentCoefficient)*dot(magnitude,abs(surface.tangent))+
        abs(normalCoefficient)*dot(magnitude,abs(surface.normal));
    float bound=centerZ+radius,margin=1e-5*max(1.0,scale);
    return !isnan(bound) && !isinf(bound) && !isnan(margin) && !isinf(margin) && bound < -margin;
}
uint LtcIntegralMask(LtcSurface surface,vec3 albedo,vec3 position,vec3 center,
    vec3 axisX,vec3 axisY,bool disc)
{
    uint mask=0u;
    if(any(notEqual(surface.specularWeight,vec3(0))) &&
        !LtcBelowHorizon(surface,position,center,axisX,axisY,surface.inverseMatrix.y,surface.inverseMatrix.w,disc))
        mask|=1u;
    if(any(notEqual(surface.diffuseWeight*albedo,vec3(0))) &&
        !LtcBelowHorizon(surface,position,center,axisX,axisY,0.0,1.0,disc))
        mask|=2u;
    return mask;
}
vec3 EvaluateLtcPolygon(LtcSurface surface,vec3 position,vec3 center,vec3 axisX,vec3 axisY,
    bool disc,uint integralMask,vec3 albedo,vec3 radiance)
{
    // Both integrals share the surface-basis projection and retain their own inverse map.
    vec3 localCenter=LtcProject(center-position,surface.tangent,surface.bitangent,surface.normal);
    vec3 localX=LtcProject(axisX,surface.tangent,surface.bitangent,surface.normal);
    vec3 localY=LtcProject(axisY,surface.tangent,surface.bitangent,surface.normal);
    float specular=0,diffuse=0;
    if((integralMask&1u)!=0u)specular=LtcPolygonIntegral(surface.inverseMatrix,localCenter,localX,localY,disc);
    if((integralMask&2u)!=0u)diffuse=LtcPolygonIntegral(vec4(1,0,0,1),localCenter,localX,localY,disc);
    // 정규화된 cosine 적분이므로 diffuse에 1/pi나 거리 감쇠를 다시 곱하지 않는다.
    return radiance*(surface.specularWeight*specular+surface.diffuseWeight*albedo*diffuse);
}
vec3 EvaluateRectAreaLight(RendererRectAreaLight light,LtcSurface surface,vec3 albedo,vec3 worldPosition)
{
    vec3 direction=normalize(light.directionWidth.xyz);
    if(light.directionWidth.w<=0 || light.upHeight.w<=0 || dot(direction,worldPosition-light.positionIntensity.xyz)<=0)
        return vec3(0);
    vec3 right=normalize(cross(direction,light.upHeight.xyz));
    vec3 up=normalize(cross(right,direction));
    vec3 x=right*(light.directionWidth.w*.5),y=up*(light.upHeight.w*.5),center=light.positionIntensity.xyz;
    uint integralMask=LtcIntegralMask(surface,albedo,worldPosition,center,x,y,false);
    if(integralMask==0u)return vec3(0);
    return EvaluateLtcPolygon(surface,worldPosition,center,x,y,false,integralMask,albedo,
        light.color.rgb*max(light.positionIntensity.w,0.0));
}
vec3 EvaluateDiscAreaLight(RendererDiscAreaLight light,LtcSurface surface,vec3 albedo,vec3 worldPosition)
{
    vec3 direction=normalize(light.directionRadius.xyz);
    if(light.directionRadius.w<=0 || dot(direction,worldPosition-light.positionIntensity.xyz)<=0)return vec3(0);
    vec3 right=normalize(cross(direction,light.up.xyz));
    vec3 up=normalize(cross(right,direction));
    vec3 x=right*light.directionRadius.w,y=up*light.directionRadius.w;
    uint integralMask=LtcIntegralMask(surface,albedo,worldPosition,light.positionIntensity.xyz,x,y,true);
    if(integralMask==0u)return vec3(0);
    // Keep the original 16-sided disc boundary and stream it in LTC space.
    return EvaluateLtcPolygon(surface,worldPosition,light.positionIntensity.xyz,x,y,true,integralMask,albedo,
        light.color.rgb*max(light.positionIntensity.w,0.0));
}
#endif

vec3 EvaluateLights(RendererLightingConstants lighting, vec3 worldPosition, vec3 normal, vec3 viewDir,
    vec3 albedo, float roughness,DirectSurface directSurface) {
    vec3 directLight = vec3(0.0,0.0,0.0);
    float ndotv=directSurface.ndotv;
    int directionalCount = clamp(int(lighting.lightCounts.x + 0.5), 0, RENDERER_MAX_DIRECTIONAL_LIGHTS);
    for (int lightIndex = 0; lightIndex < directionalCount; ++lightIndex) {
        RendererDirectionalLight light = lighting.directionalLights[lightIndex];
        vec3 lightDir = normalize(light.directionIntensity.xyz);
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
    uint localCount=listed==0xffffffffu?clusters.counts.x:listed;
    for(uint item=0u;item<localCount;++item)
    {
        uint lightIndex=listed==0xffffffffu?item:clusterList[base+1u+item];
        ClusterLight light=clusterLights[lightIndex];
        vec3 toLight=light.positionRange.xyz-worldPosition;
        float distanceToLight=length(toLight);
        if(distanceToLight<=0.0001 || distanceToLight>=light.positionRange.w)continue;
        vec3 lightDir=toLight/distanceToLight;
        if(ndotv<=0.0 || max(dot(normal,lightDir),0.0)<=0.0)continue;
        float attenuation=PunctualAttenuation(distanceToLight,light.positionRange.w);
        int type=int(light.coneShadow.y+0.5),shadowIndex=int(light.coneShadow.z);
        if(type==3)attenuation*=SpotConeAttenuation(dot(-lightDir,light.directionOuterCos.xyz),
            light.coneShadow.x,light.directionOuterCos.w);
        if(attenuation<=0.0)continue;
        vec3 incidentIlluminance=light.colorIntensity.rgb*max(light.colorIntensity.w,0.0)*attenuation;
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

void main() {
    int textureFlags = int(pushConstants.textureFlags);
    vec4 base = pushConstants.baseColor * fragColor;
    if ((textureFlags & RENDERER_TEXTURE_FLAG_BASE_COLOR) != 0) {
        base *= SampleMaterial(0u,fragUv);
    }
    uint alphaMode=pushConstants.padding1 & 3u;
    // Renderer-owned bit 6: texture alpha still defines cutout coverage, but
    // its RGB and all lighting are excluded from the LOD display color.
    if((pushConstants.textureFlags&64u)!=0u)
    {
        if(alphaMode==1u && base.a<uintBitsToFloat(pushConstants.padding0)) discard;
        vec3 color=pushConstants.baseColor.rgb;
        if(lighting.pbrParams.z>.5) color=pow(color,vec3(1.0/2.2));
        outColor=vec4(color,alphaMode==2u ? base.a : 1.0);return;
    }
    if((pushConstants.padding1&16u)!=0u)
    {
        if(alphaMode==1u && base.a<uintBitsToFloat(pushConstants.padding0)) discard;
        vec3 color=base.rgb;
        if(lighting.pbrParams.z>=0.0)
        {
            color=min(color*lighting.pbrParams.w,vec3(16777216.0));
            color=color/(color+vec3(1));
            if(lighting.pbrParams.z>.5)color=pow(color,vec3(1.0/2.2));
        }
        color=vec3(isnan(color.r)?0:clamp(color.r,0,65504),isnan(color.g)?0:clamp(color.g,0,65504),
            isnan(color.b)?0:clamp(color.b,0,65504));
        outColor=vec4(color,alphaMode==2u?base.a:1.0);return;
    }
    vec3 albedo=base.rgb;

    float minRoughness = clamp(lighting.pbrParams.x, 0.01, 1.0);
    float ambientSpecularStrength = max(lighting.pbrParams.y, 0.0);
    float metallic = clamp(pushConstants.materialParams.x, 0.0, 1.0);
    float roughness = clamp(pushConstants.materialParams.y, minRoughness, 1.0);
    float occlusion = 1.0;
    // Opaque environment buffers already include authored AO and screen AO.
    bool usesEnvironmentBuffers=environment.settings.x>0.5 && alphaMode!=2u;
    if ((textureFlags & RENDERER_TEXTURE_FLAG_METALLIC_ROUGHNESS) != 0) {
        vec4 metallicRoughness = SampleMaterial(1u,fragUv);
        roughness = clamp(roughness * metallicRoughness.g, minRoughness, 1.0);
        metallic = clamp(metallic * metallicRoughness.b, 0.0, 1.0);
    }
    if ((textureFlags & RENDERER_TEXTURE_FLAG_OCCLUSION) != 0 && !usesEnvironmentBuffers) {
        occlusion = mix(1.0, SampleMaterial(3u,fragUv).r, clamp(pushConstants.materialParams.w, 0.0, 1.0));
    }

    vec3 normal = GetNormal();
    // Pixel-normal footprint filtering matches Surface and transparent shading.
    // Evaluate derivatives before alpha discard and per-face orientation changes.
    vec3 normalDx=dFdx(normal),normalDy=dFdy(normal);
    float normalVariance=0.25*(dot(normalDx,normalDx)+dot(normalDy,normalDy));
    float filteredAlphaSquared=min(1.0,roughness*roughness*roughness*roughness+min(2.0*normalVariance,0.18));
    roughness=sqrt(sqrt(filteredAlphaSquared));
    if(alphaMode==1u && base.a<uintBitsToFloat(pushConstants.padding0)) discard;
    if((pushConstants.padding1 & 4u)!=0u && !gl_FrontFacing) normal=-normal;
    vec3 viewDir = normalize(lighting.cameraPosition.xyz - fragWorldPosition);
    DirectSurface directSurface=PrepareDirectSurface(normal,viewDir,albedo,metallic,roughness);
    vec3 f0=directSurface.f0;
    vec3 directLight = EvaluateLights(lighting, fragWorldPosition, normal, viewDir, albedo, roughness,directSurface);
    vec3 ambientFresnel = FresnelSchlickRoughness(directSurface.ndotv, f0, roughness);
    vec3 kD = (vec3(1.0) - ambientFresnel) * (1.0 - metallic);
    vec3 ambientDiffuse = kD * albedo * lighting.ambientColor.rgb * lighting.ambientColor.a;
    vec3 ambientSpecular = ambientFresnel * lighting.environmentColor.rgb * lighting.environmentColor.a * ambientSpecularStrength;
    vec3 ambient = (ambientDiffuse + ambientSpecular) * occlusion;
    vec2 screenUV=gl_FragCoord.xy*environment.screen.xy;
    float screenAO=!usesEnvironmentBuffers && environment.screen.z>0.5 ? texture(sampler2D(screenAOTexture,screenSampler),screenUV).r : 1.0;
    ambient*=screenAO;
    if(environment.settings.x>0.5)
    {
        ambient=vec3(0);
        if(alphaMode==2u)
        {
            if((uint(environment.screen.w+0.5)&1u)!=0u)
                ambient=kD*albedo*EnvironmentIrradiance(normal)*(0.3183098861837907*environment.settings.y*occlusion*screenAO);
        }
        else
        {
            ambient=kD*albedo*texture(sampler2D(environmentDiffuseTexture,screenSampler),screenUV).rgb;
            ambient+=texture(sampler2D(environmentSpecularTexture,screenSampler),screenUV).rgb;
        }
    }
    vec3 emissive = pushConstants.emissiveColor.rgb;
    if ((textureFlags & RENDERER_TEXTURE_FLAG_EMISSIVE) != 0) {
        emissive *= SampleMaterial(4u,fragUv).rgb;
    }
    vec3 color = lighting.lightCounts.w > 0.5 ? ambient + directLight + emissive : albedo + emissive;

    if(lighting.pbrParams.z >= 0.0) {
        color = min(color * lighting.pbrParams.w, vec3(16777216.0));
    color = color / (color + vec3(1.0));
    if (lighting.pbrParams.z > 0.5) color = pow(color, vec3(1.0 / 2.2));
    }
    // HDR scene storage is RGBA16F. Keep an over-range highlight from storing
    // infinity and contaminating GI, bloom and temporal neighborhood statistics.
    color=vec3(isnan(color.r)?0.0:clamp(color.r,0.0,65504.0),
        isnan(color.g)?0.0:clamp(color.g,0.0,65504.0),isnan(color.b)?0.0:clamp(color.b,0.0,65504.0));
    outColor = vec4(color, alphaMode==2u ? base.a : 1.0);
}
