// mesh_ps.hlsl - D3D12 Renderer lighting path matching the Vulkan GLSL shader.

// 이 셰이더가 사용하는 바인딩과 입력 크기다.
#ifndef RENDERER_BINDLESS
#define RENDERER_BINDLESS 0
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

#if !RENDERER_BINDLESS
Texture2D BaseColorTexture : register(t0, space0);
#endif
#if !RENDERER_BINDLESS
Texture2D MetallicRoughnessTexture : register(t4, space0);
#endif
#if !RENDERER_BINDLESS
Texture2D NormalTexture : register(t5, space0);
#endif
#if !RENDERER_BINDLESS
Texture2D OcclusionTexture : register(t6, space0);
#endif
#if !RENDERER_BINDLESS
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

float DistributionGGX(float3 normal, float3 halfway, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float ndoth = max(dot(normal, halfway), 0.0);
    float ndoth2 = ndoth * ndoth;
    float denom = ndoth2 * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 0.0001);
}

float GeometrySchlickGGX(float ndotv, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return ndotv / max(ndotv * (1.0 - k) + k, 0.0001);
}

float GeometrySmith(float3 normal, float3 viewDir, float3 lightDir, float roughness)
{
    float ndotv = max(dot(normal, viewDir), 0.0);
    float ndotl = max(dot(normal, lightDir), 0.0);
    return GeometrySchlickGGX(ndotv, roughness) * GeometrySchlickGGX(ndotl, roughness);
}

float3 FresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0 - f0) * pow(saturate(1.0 - cosTheta), 5.0);
}

float3 FresnelSchlickRoughness(float cosTheta, float3 f0, float roughness)
{
    float3 roughnessF0 = max(float3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness), f0);
    return f0 + (roughnessF0 - f0) * pow(saturate(1.0 - cosTheta), 5.0);
}

#if RENDERER_BINDLESS
Texture2D materialTextures[28]:register(t0, space0);
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

float4 SampleBaseColorTexture(float2 uv)
{
    return SampleMaterial(0u,uv);
}

float3 GetNormal(PSInput input, uint textureFlags)
{
    float3 normal = normalize(input.worldNormal);
    if ((textureFlags & RENDERER_TEXTURE_FLAG_NORMAL) == 0u)
    {
        return normal;
    }
    float normalScale = materialParams.z;
    if (normalScale <= 0.0001)
    {
        return normal;
    }
    float3 tangent = normalize(input.worldTangent.xyz - normal * dot(normal, input.worldTangent.xyz));
    float3 bitangent = normalize(cross(normal, tangent)) * input.worldTangent.w;
    float3x3 tbn = float3x3(tangent, bitangent, normal);
    float3 tangentNormal = SampleMaterial(2u,input.uv).xyz * 2.0 - 1.0;
    tangentNormal.xy *= normalScale;
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
};
cbuffer ShadowMatrices:register(b3, space0) {ShadowData shadowMatrix;};

float SampleShadow(int type,int lightIndex,float3 worldPosition,float3 normal,float3 lightDir )
{
    if((drawTextureFlags & RENDERER_TEXTURE_FLAG_RECEIVE_SHADOW)==0u)return 1.0;
    float4 views=type==1?shadowMatrix.directionalViews[lightIndex]:(type==2?shadowMatrix.pointViews[lightIndex]:shadowMatrix.spotViews[lightIndex]);
    int count=int(views.y+.5);
    if(count==0)return 1.0;
    int local=0;
    if(type==1 && count>1)
    {
        float depth=-mul(shadowMatrix.cameraView,float4(worldPosition,1.0)).z;
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
            float sampled=ShadowMap.Sample(ShadowSampler,sampleUv).r;
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
        visible+=receiver<=ShadowMap.Sample(ShadowSampler,sampleUv).r?1.0:0.0;
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
    float3 f0 = lerp(float3(0.04,0.04,0.04), albedo, metallic);
    float ndf = DistributionGGX(normal, halfway, roughness);
    float geometry = GeometrySmith(normal, viewDir, lightDir, roughness);
    float3 fresnel = FresnelSchlick(max(dot(halfway, viewDir), 0.0), f0);
    float3 specular = (ndf * geometry * fresnel) / max(4.0 * ndotv * ndotl, 0.0001);
    float3 diffuseWeight = (float3(1.0,1.0,1.0) - fresnel) * (1.0 - metallic);
    return (diffuseWeight * albedo / PI + specular) * incidentIlluminance * ndotl * visibility;
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
    float sampleArea = PI * radius * radius / float(sampleCount);
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

inline float3 EvaluateLights(RendererLightingConstants lighting, float3 worldPosition, float3 normal, float3 viewDir,
    float3 albedo, float metallic, float roughness) {
    float3 directLight = float3(0.0,0.0,0.0);
    int directionalCount = clamp(int(lighting.lightCounts.x + 0.5), 0, RENDERER_MAX_DIRECTIONAL_LIGHTS);
    for (int lightIndex = 0; lightIndex < directionalCount; ++lightIndex) {
        RendererDirectionalLight light = lighting.directionalLights[lightIndex];
        float3 lightDir = normalize(light.directionIntensity.xyz);
#if RENDERER_ENABLE_SHADOWS
        float visibility = SampleShadow(1,lightIndex,worldPosition,normal,lightDir);
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
        float visibility = SampleShadow(2,lightIndex,worldPosition,normal,lightDir);
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
        float visibility = SampleShadow(3,lightIndex,worldPosition,normal,lightDir);
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

float4 main(PSInput input) : SV_TARGET
{
    uint textureFlags = drawTextureFlags;
    float3 albedo = baseColor.rgb;
    if ((textureFlags & RENDERER_TEXTURE_FLAG_BASE_COLOR) != 0u)
    {
        albedo *= SampleBaseColorTexture(input.uv).rgb;
    }

    float minRoughness = clamp(lighting.pbrParams.x, 0.01, 1.0);
    float ambientSpecularStrength = max(lighting.pbrParams.y, 0.0);
    float metallic = clamp(materialParams.x, 0.0, 1.0);
    float roughness = clamp(materialParams.y, minRoughness, 1.0);
    float occlusion = clamp(materialParams.w, 0.0, 1.0);
    if ((textureFlags & RENDERER_TEXTURE_FLAG_METALLIC_ROUGHNESS) != 0u)
    {
        float4 metallicRoughness = SampleMaterial(1u,input.uv);
        roughness = clamp(roughness * metallicRoughness.g, minRoughness, 1.0);
        metallic = clamp(metallic * metallicRoughness.b, 0.0, 1.0);
    }
    if ((textureFlags & RENDERER_TEXTURE_FLAG_OCCLUSION) != 0u)
    {
        occlusion *= SampleMaterial(3u,input.uv).r;
    }
    float3 emissive = emissiveColor.rgb;
    if ((textureFlags & RENDERER_TEXTURE_FLAG_EMISSIVE) != 0u)
    {
        emissive *= SampleMaterial(4u,input.uv).rgb;
    }

    float3 normal = GetNormal(input, textureFlags);
    float3 viewDir = normalize(lighting.cameraPosition.xyz - input.worldPosition);
    bool usePointLight = lighting.pointLightColorIntensity.a > 0.0 && lighting.pointLightPositionRange.w > 0.0;
    float3 lightDir = normalize(lighting.directionalLightDirection.xyz);
    if (usePointLight)
    {
        float3 toLight = lighting.pointLightPositionRange.xyz - input.worldPosition;
        float distanceToLight = length(toLight);
        lightDir = distanceToLight > 0.0001 ? toLight / distanceToLight : float3(0.0, 0.0, 1.0);
    }

    float3 halfway = normalize(viewDir + lightDir);
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 fresnel = FresnelSchlick(max(dot(halfway, viewDir), 0.0), f0);

    float3 kS = fresnel;
    float3 kD = (float3(1.0, 1.0, 1.0) - kS) * (1.0 - metallic);
    float3 directLight = EvaluateLights(lighting, input.worldPosition, normal, viewDir, albedo, metallic, roughness);
    float3 ambientFresnel = FresnelSchlickRoughness(max(dot(normal, viewDir), 0.0), f0, roughness);
    float3 ambientDiffuse = kD * albedo * lighting.ambientColor.rgb * lighting.ambientColor.a;
    float3 ambientSpecular = ambientFresnel * lighting.environmentColor.rgb * lighting.environmentColor.a * ambientSpecularStrength;
    float3 ambient = (ambientDiffuse + ambientSpecular) * occlusion;
    float3 color = lighting.lightCounts.w > 0.5 ? ambient + directLight + emissive : albedo + emissive;

    if(lighting.pbrParams.z >= 0.0) {
        color *= lighting.pbrParams.w;
    color = color / (color + float3(1.0, 1.0, 1.0));
    if (lighting.pbrParams.z > 0.5) color = pow(color, float3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2));
    }
    return float4(color, baseColor.a);
}
