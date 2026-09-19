#include <metal_stdlib>
// 이 셰이더가 사용하는 바인딩과 입력 크기다.
#ifndef RENDERER_BINDLESS
#define RENDERER_BINDLESS 0
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
#if RENDERER_ENABLE_SHADOWS
constant uint kTextureFlagReceiveShadow =
    uint(RENDERER_TEXTURE_FLAG_RECEIVE_SHADOW);
#endif
constant float kPi = 3.14159265359f;

inline float DistributionGGX(float3 normal, float3 halfway, float roughness)
{
    const float a = roughness * roughness;
    const float a2 = a * a;
    const float ndoth = max(dot(normal, halfway), 0.0f);
    const float ndoth2 = ndoth * ndoth;
    const float denominator = ndoth2 * (a2 - 1.0f) + 1.0f;
    return a2 / max(kPi * denominator * denominator, 0.0001f);
}

inline float GeometrySchlickGGX(float ndotv, float roughness)
{
    const float r = roughness + 1.0f;
    const float k = (r * r) / 8.0f;
    return ndotv / max(ndotv * (1.0f - k) + k, 0.0001f);
}

inline float GeometrySmith(
    float3 normal,
    float3 viewDirection,
    float3 lightDirection,
    float roughness)
{
    const float ndotv = max(dot(normal, viewDirection), 0.0f);
    const float ndotl = max(dot(normal, lightDirection), 0.0f);
    return GeometrySchlickGGX(ndotv, roughness) *
        GeometrySchlickGGX(ndotl, roughness);
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
};

float SampleShadow(int type,int lightIndex,float3 worldPosition,float3 normal,float3 lightDir ,constant RendererLightingConstants& lighting,constant ShadowMatrix& shadowMatrix,depth2d<float> shadowMap,sampler shadowSampler,uint drawFlags)
{
    if((drawFlags & RENDERER_TEXTURE_FLAG_RECEIVE_SHADOW)==0u)return 1.0;
    float4 views=type==1?shadowMatrix.directionalViews[lightIndex]:(type==2?shadowMatrix.pointViews[lightIndex]:shadowMatrix.spotViews[lightIndex]);
    int count=int(views.y+.5);
    if(count==0)return 1.0;
    int local=0;
    if(type==1 && count>1)
    {
        float depth=-(shadowMatrix.cameraView*float4(worldPosition,1.0)).z;
        while(local<count-1 && depth>shadowMatrix.directionalSplits[lightIndex][local])++local;
    }
    if(type==2)
    {
        float3 direction=-lightDir;
        float3 magnitude=abs(direction);
        if(magnitude.x>=magnitude.y && magnitude.x>=magnitude.z)local=direction.x>=0?0:1;
        else if(magnitude.y>=magnitude.z)local=direction.y>=0?2:3;
        else local=direction.z>=0?4:5;
    }
    int index=int(views.x+.5)+local;
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
    uv=tile.xy+uv*tile.zw;
    float angle=1.0-max(dot(normal,lightDir),0.0);
    float bias=max(lighting.shadowParams.x,lighting.shadowParams.y*angle)+lighting.shadowParams.z*angle;
    float receiver=ndc.z-bias;
    float radius=lighting.shadowParams.w;
    if(shadowMatrix.filterParams.x>0.0 && shadowMatrix.filterParams.y>0.0)
    {
        float blockers=0.0,blockerDepth=0.0;
        for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x)
        {
            float2 sampleUv=clamp(uv+float2(x,y)*texel*shadowMatrix.filterParams.y,minUv,maxUv);
            float sampled=shadowMap.sample(shadowSampler,sampleUv);
            if(sampled<receiver){blockerDepth+=sampled;blockers+=1.0;}
        }
        if(blockers>0.0)
        {
            blockerDepth/=blockers;
            radius=max(radius,shadowMatrix.filterParams.x*max(receiver-blockerDepth,0.0)/max(blockerDepth,.0001)/texel.x);
        }
    }
    radius=min(radius,shadowMatrix.filterParams.z);
    int extent=int(ceil(radius));
    float visible=0.0,samples=0.0;
    for(int y=-extent;y<=extent;++y)for(int x=-extent;x<=extent;++x)
    {
        float2 sampleUv=clamp(uv+float2(x,y)*texel,minUv,maxUv);
        visible+=receiver<=shadowMap.sample(shadowSampler,sampleUv)?1.0:0.0;
        samples+=1.0;
    }
    return 1.0-clamp(views.z,0.0,1.0)*(1.0-visible/max(samples,1.0));
}

#else
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
    float geometry = GeometrySmith(normal, viewDir, lightDir, roughness);
    float3 fresnel = FresnelSchlick(max(dot(halfway, viewDir), 0.0), f0);
    float3 specular = (ndf * geometry * fresnel) / max(4.0 * ndotv * ndotl, 0.0001);
    float3 diffuseWeight = (float3(1.0,1.0,1.0) - fresnel) * (1.0 - metallic);
    return (diffuseWeight * albedo / kPi + specular) * incidentIlluminance * ndotl * visibility;
}

inline float3 EvaluateAreaSample(
    float3 normal,
    float3 viewDir,
    float3 samplePosition,
    float3 emitterDirection,
    float3 emittedRadiance,
    float sampleArea,
    float3 albedo,
    float metallic,
    float roughness,
    float3 worldPosition) {
    float3 toLight = samplePosition - worldPosition;
    float distanceSquared = dot(toLight, toLight);
    if (distanceSquared <= 0.000001 || sampleArea <= 0.0) {
        return float3(0.0,0.0,0.0);
    }
    float3 lightDir = toLight * rsqrt(distanceSquared);
    float emitterCosine = max(dot(emitterDirection, -lightDir), 0.0);
    float3 incidentIlluminance = emittedRadiance * emitterCosine * sampleArea / distanceSquared;
    return EvaluateDirectLight(normal, viewDir, lightDir, incidentIlluminance, albedo, metallic, roughness, 1.0);
}

inline float3 EvaluateRectAreaLight(
    RendererRectAreaLight light,
    float3 normal,
    float3 viewDir,
    float3 albedo,
    float metallic,
    float roughness,
    float3 worldPosition) {
    float3 emitterDirection = normalize(light.directionWidth.xyz);
    float3 emitterUp = normalize(light.upHeight.xyz);
    float3 emitterRight = normalize(cross(emitterDirection, emitterUp));
    emitterUp = normalize(cross(emitterRight, emitterDirection));
    float width = max(light.directionWidth.w, 0.0);
    float height = max(light.upHeight.w, 0.0);
    float sampleArea = width * height * 0.25;
    float3 emittedRadiance = light.color.rgb * max(light.positionIntensity.w, 0.0);
    float3 result = float3(0.0,0.0,0.0);
    for (int sampleIndex = 0; sampleIndex < 4; ++sampleIndex) {
        float x = (sampleIndex & 1) == 0 ? -0.25 : 0.25;
        float y = (sampleIndex & 2) == 0 ? -0.25 : 0.25;
        float3 samplePosition = light.positionIntensity.xyz + emitterRight * (x * width) + emitterUp * (y * height);
        result += EvaluateAreaSample(normal, viewDir, samplePosition, emitterDirection, emittedRadiance, sampleArea, albedo, metallic, roughness, worldPosition);
    }
    return result;
}

inline float3 EvaluateDiscAreaLight(
    RendererDiscAreaLight light,
    float3 normal,
    float3 viewDir,
    float3 albedo,
    float metallic,
    float roughness,
    float3 worldPosition) {
    const int sampleCount = 8;
    const float goldenAngle = 2.39996323;
    float3 emitterDirection = normalize(light.directionRadius.xyz);
    float3 emitterUp = normalize(light.up.xyz);
    float3 emitterRight = normalize(cross(emitterDirection, emitterUp));
    emitterUp = normalize(cross(emitterRight, emitterDirection));
    float radius = max(light.directionRadius.w, 0.0);
    float sampleArea = kPi * radius * radius / float(sampleCount);
    float3 emittedRadiance = light.color.rgb * max(light.positionIntensity.w, 0.0);
    float3 result = float3(0.0,0.0,0.0);
    for (int sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
        float normalizedRadius = sqrt((float(sampleIndex) + 0.5) / float(sampleCount));
        float angle = float(sampleIndex) * goldenAngle;
        float2 discOffset = float2(cos(angle), sin(angle)) * (normalizedRadius * radius);
        float3 samplePosition = light.positionIntensity.xyz + emitterRight * discOffset.x + emitterUp * discOffset.y;
        result += EvaluateAreaSample(normal, viewDir, samplePosition, emitterDirection, emittedRadiance, sampleArea, albedo, metallic, roughness, worldPosition);
    }
    return result;
}

inline float3 EvaluateLights(constant RendererLightingConstants& lighting,
#if RENDERER_ENABLE_SHADOWS
    constant ShadowMatrix& shadowMatrix, depth2d<float> shadowMap, sampler shadowSampler, uint drawFlags,
#endif
    float3 worldPosition, float3 normal, float3 viewDir,
    float3 albedo, float metallic, float roughness) {
    float3 directLight = float3(0.0,0.0,0.0);
    int directionalCount = clamp(int(lighting.lightCounts.x + 0.5), 0, RENDERER_MAX_DIRECTIONAL_LIGHTS);
    for (int lightIndex = 0; lightIndex < directionalCount; ++lightIndex) {
        RendererDirectionalLight light = lighting.directionalLights[lightIndex];
        float3 lightDir = normalize(light.directionIntensity.xyz);
#if RENDERER_ENABLE_SHADOWS
        float visibility = SampleShadow(1,lightIndex,worldPosition,normal,lightDir,lighting,shadowMatrix,shadowMap,shadowSampler,drawFlags);
#else
        float visibility = 1.0;
#endif
        directLight += EvaluateDirectLight(normal, viewDir, lightDir,
            light.color.rgb * max(light.directionIntensity.w, 0.0), albedo, metallic, roughness, visibility);
    }
    int pointCount = clamp(int(lighting.lightCounts.y + 0.5), 0, RENDERER_MAX_POINT_LIGHTS);
    for (int lightIndex = 0; lightIndex < pointCount; ++lightIndex) {
        RendererPointLight light = lighting.pointLights[lightIndex];
        float3 toLight = light.positionRange.xyz - worldPosition;
        float distanceToLight = length(toLight);
        if (distanceToLight <= 0.0001) {
            continue;
        }
        float3 lightDir = toLight / distanceToLight;
#if RENDERER_ENABLE_SHADOWS
        float visibility = SampleShadow(2,lightIndex,worldPosition,normal,lightDir,lighting,shadowMatrix,shadowMap,shadowSampler,drawFlags);
#else
        float visibility = 1.0;
#endif
        float3 incidentIlluminance = light.colorIntensity.rgb * max(light.colorIntensity.w, 0.0) *
            PunctualAttenuation(distanceToLight, light.positionRange.w);
        directLight += EvaluateDirectLight(normal, viewDir, lightDir, incidentIlluminance, albedo, metallic, roughness, visibility);
    }
    int spotCount = clamp(int(lighting.lightCounts.z + 0.5), 0, RENDERER_MAX_SPOT_LIGHTS);
    for (int lightIndex = 0; lightIndex < spotCount; ++lightIndex) {
        RendererSpotLight light = lighting.spotLights[lightIndex];
        float3 toLight = light.positionRange.xyz - worldPosition;
        float distanceToLight = length(toLight);
        if (distanceToLight <= 0.0001) {
            continue;
        }
        float3 lightDir = toLight / distanceToLight;
        float attenuation = PunctualAttenuation(distanceToLight, light.positionRange.w) *
            SpotConeAttenuation(dot(-lightDir, normalize(light.directionOuterCos.xyz)),
                light.coneParams.x, light.directionOuterCos.w);
        float3 incidentIlluminance = light.colorIntensity.rgb * max(light.colorIntensity.w, 0.0) * attenuation;
#if RENDERER_ENABLE_SHADOWS
        float visibility = SampleShadow(3,lightIndex,worldPosition,normal,lightDir,lighting,shadowMatrix,shadowMap,shadowSampler,drawFlags);
#else
        float visibility = 1.0;
#endif
        directLight += EvaluateDirectLight(normal, viewDir, lightDir, incidentIlluminance, albedo, metallic, roughness, visibility);
    }
    int rectAreaCount = clamp(int(lighting.areaLightCounts.x + 0.5), 0, RENDERER_MAX_RECT_AREA_LIGHTS);
    for (int lightIndex = 0; lightIndex < rectAreaCount; ++lightIndex) {
        directLight += EvaluateRectAreaLight(lighting.rectAreaLights[lightIndex], normal, viewDir, albedo, metallic, roughness, worldPosition);
    }
    int discAreaCount = clamp(int(lighting.areaLightCounts.y + 0.5), 0, RENDERER_MAX_DISC_AREA_LIGHTS);
    for (int lightIndex = 0; lightIndex < discAreaCount; ++lightIndex) {
        directLight += EvaluateDiscAreaLight(lighting.discAreaLights[lightIndex], normal, viewDir, albedo, metallic, roughness, worldPosition);
    }

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
        drawConstants.materialParams.z <= 0.0001f)
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
    return normalize(tangentBasis * tangentNormal);
}

inline float4 RunFragmentShader(
    RasterData input,
    constant DrawConstants& drawConstants,
    constant RendererLightingConstants& lighting,
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
    sampler materialSampler)
{
    const uint textureFlags = drawConstants.textureFlags;
    float3 albedo = drawConstants.baseColor.rgb;
    if ((textureFlags & kTextureFlagBaseColor) != 0u)
    {
        albedo *= baseColorTexture.sample(materialSampler, input.uv).rgb;
    }

    const float minRoughness =
        clamp(lighting.pbrParams.x, 0.01f, 1.0f);
    const float ambientSpecularStrength =
        max(lighting.pbrParams.y, 0.0f);
    float metallic =
        clamp(drawConstants.materialParams.x, 0.0f, 1.0f);
    float roughness =
        clamp(drawConstants.materialParams.y, minRoughness, 1.0f);
    float occlusion =
        clamp(drawConstants.materialParams.w, 0.0f, 1.0f);

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
    if ((textureFlags & kTextureFlagOcclusion) != 0u)
    {
        occlusion *=
            occlusionTexture.sample(materialSampler, input.uv).r;
    }

    const float3 normal =
        ResolveNormal(input, drawConstants, normalTexture, materialSampler);
    const float3 viewDirection =
        normalize(lighting.cameraPosition.xyz - input.worldPosition);

    const bool usePointLight =
        lighting.pointLightColorIntensity.a > 0.0f &&
        lighting.pointLightPositionRange.w > 0.0f;
    float3 lightDirection =
        normalize(lighting.directionalLightDirection.xyz);

    if (usePointLight)
    {
        const float3 toLight =
            lighting.pointLightPositionRange.xyz - input.worldPosition;
        const float distanceToLight = length(toLight);
        lightDirection = distanceToLight > 0.0001f
            ? toLight / distanceToLight
            : float3(0.0f, 0.0f, 1.0f);
    }

    const float3 halfway =
        normalize(viewDirection + lightDirection);
    const float3 f0 =
        mix(float3(0.04f), albedo, metallic);
    const float3 fresnel =
        FresnelSchlick(max(dot(halfway, viewDirection), 0.0f), f0);

    const float3 kS = fresnel;
    const float3 kD =
        (float3(1.0f) - kS) * (1.0f - metallic);
    const float3 directLight = EvaluateLights(lighting,
#if RENDERER_ENABLE_SHADOWS
        shadowMatrix,shadowMap,shadowSampler,drawConstants.textureFlags,
#endif
        input.worldPosition, normal, viewDirection, albedo, metallic, roughness);

    const float3 ambientFresnel = FresnelSchlickRoughness(
        max(dot(normal, viewDirection), 0.0f),
        f0,
        roughness);
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
    const float3 ambient =
        (ambientDiffuse + ambientSpecular) * occlusion;

    float3 emissive = drawConstants.emissiveColor.rgb;
    if ((textureFlags & kTextureFlagEmissive) != 0u)
    {
        emissive *=
            emissiveTexture.sample(materialSampler, input.uv).rgb;
    }

    float3 color = lighting.lightCounts.w > 0.5 ? ambient + directLight + emissive : albedo + emissive;
    if(lighting.pbrParams.z >= 0.0f) {
        color *= lighting.pbrParams.w;
        color = color / (color + 1.0f);
        if (lighting.pbrParams.z > 0.5f) color = pow(color, float3(1.0f / 2.2f));
    }
    return float4(color, drawConstants.baseColor.a);
}

fragment float4 RENDERER_FRAGMENT_ENTRY(
    RasterData input [[stage_in]],
#if RENDERER_BINDLESS
    array<texture2d<float>,28> materialTextures [[texture(0)]],
    const device uint4* materialTextureIndices [[buffer(29)]],
#endif
    constant DrawConstants& drawConstants [[buffer(RENDERER_BINDING_INLINE_CONSTANTS)]],
    constant RendererLightingConstants& lighting [[buffer(RENDERER_BINDING_LIGHTING_CONSTANTS)]],
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
        drawConstants,
        lighting,
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
        materialSampler);
}
