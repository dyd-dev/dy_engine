// mesh_ps.hlsl - D3D12 Renderer lighting path matching the Vulkan GLSL shader.

#include "Graphics/ShaderInterop/StockShaderLayout.inc"

#ifndef RENDERER_ENABLE_SHADOWS
#error RENDERER_ENABLE_SHADOWS must be defined
#endif

#define REGISTER_TOKEN_IMPL(prefix, index) prefix##index
#define REGISTER_TOKEN(prefix, index) REGISTER_TOKEN_IMPL(prefix, index)
#define BUFFER_REGISTER(index) REGISTER_TOKEN(b, index)
#define TEXTURE_REGISTER(index) REGISTER_TOKEN(t, index)
#define REGISTER_SPACE(index) REGISTER_TOKEN(space, index)

cbuffer DrawConstants : register(
    BUFFER_REGISTER(RENDERER_BINDING_INLINE_CONSTANTS),
    REGISTER_SPACE(RENDERER_DESCRIPTOR_SET))
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

#define DY_LIGHT_FLOAT4 float4
#include "Graphics/ShaderInterop/LightingTypes.inc"
#undef DY_LIGHT_FLOAT4
cbuffer RendererLighting : register(b1, space0) { RendererLightingConstants lighting; };

Texture2D BaseColorTexture : register(
    TEXTURE_REGISTER(RENDERER_BINDING_BASE_COLOR_TEXTURE),
    REGISTER_SPACE(RENDERER_DESCRIPTOR_SET));
Texture2D MetallicRoughnessTexture : register(
    TEXTURE_REGISTER(RENDERER_BINDING_METALLIC_ROUGHNESS_TEXTURE),
    REGISTER_SPACE(RENDERER_DESCRIPTOR_SET));
Texture2D NormalTexture : register(
    TEXTURE_REGISTER(RENDERER_BINDING_NORMAL_TEXTURE),
    REGISTER_SPACE(RENDERER_DESCRIPTOR_SET));
Texture2D OcclusionTexture : register(
    TEXTURE_REGISTER(RENDERER_BINDING_OCCLUSION_TEXTURE),
    REGISTER_SPACE(RENDERER_DESCRIPTOR_SET));
Texture2D EmissiveTexture : register(
    TEXTURE_REGISTER(RENDERER_BINDING_EMISSIVE_TEXTURE),
    REGISTER_SPACE(RENDERER_DESCRIPTOR_SET));
#if RENDERER_ENABLE_SHADOWS
Texture2D ShadowMap : register(
    TEXTURE_REGISTER(RENDERER_BINDING_SHADOW_TEXTURE),
    REGISTER_SPACE(RENDERER_DESCRIPTOR_SET));
#endif
SamplerState LinearSampler : register(
    REGISTER_TOKEN(s, RENDERER_BINDING_MATERIAL_SAMPLER),
    REGISTER_SPACE(RENDERER_DESCRIPTOR_SET));
#if RENDERER_ENABLE_SHADOWS
SamplerState ShadowSampler : register(
    REGISTER_TOKEN(s, RENDERER_BINDING_SHADOW_SAMPLER),
    REGISTER_SPACE(RENDERER_DESCRIPTOR_SET));
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

float4 SampleBaseColorTexture(float2 uv)
{
    return BaseColorTexture.Sample(LinearSampler, uv);
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
    float3 tangentNormal = NormalTexture.Sample(LinearSampler, input.uv).xyz * 2.0 - 1.0;
    tangentNormal.xy *= normalScale;
    return normalize(mul(tangentNormal, tbn));
}

#if RENDERER_ENABLE_SHADOWS
float CalculateShadowVisibility(float4 lightSpacePosition, float3 normal, float3 lightDir)
{
    if (lighting.directionalLightDirection.w < 0.5)
        return 1.0;
    uint flags = drawTextureFlags;
    if ((flags & RENDERER_TEXTURE_FLAG_RECEIVE_SHADOW) == 0u)
        return 1.0;

    float3 ndc = lightSpacePosition.xyz / lightSpacePosition.w;
    float2 shadowUV = ndc.xy * 0.5 + 0.5;
    shadowUV.y = 1.0 - shadowUV.y;
    if (shadowUV.x < 0.0 || shadowUV.x > 1.0 ||
        shadowUV.y < 0.0 || shadowUV.y > 1.0 ||
        ndc.z < 0.0 || ndc.z > 1.0)
        return 1.0;

    float ndotl = max(dot(normal, lightDir), 0.0);
    float constantBias = max(lighting.shadowParams.x, 0.0);
    float slopeBias = max(lighting.shadowParams.y, 0.0) * (1.0 - ndotl);
    float normalBias = max(lighting.shadowParams.z, 0.0) * (1.0 - ndotl);
    float bias = max(slopeBias, constantBias) + normalBias;

    uint width, height;
    ShadowMap.GetDimensions(width, height);
    float2 texelSize = 1.0 / float2(width, height);
    int pcfRadius = (int)(lighting.shadowParams.w + 0.5);
    float visibility = 0.0;
    int sampleCount = 0;
    for (int y = -pcfRadius; y <= pcfRadius; ++y)
    {
        for (int x = -pcfRadius; x <= pcfRadius; ++x)
        {
            float sampledDepth = ShadowMap.SampleLevel(ShadowSampler, shadowUV + float2(x, y) * texelSize, 0).r;
            visibility += (ndc.z - bias) > sampledDepth ? 0.0 : 1.0;
            sampleCount += 1;
        }
    }
    visibility /= max((float)sampleCount, 1.0);
    float strength = saturate(lighting.cameraPosition.w);
    return lerp(1.0, visibility, strength);
}
#endif

#define DY_LIGHT_ARGUMENT RendererLightingConstants lighting
#define DY_LIGHT_RSQRT rsqrt
#define DY_LIGHT_MIX lerp
#define DY_LIGHT_PI PI
#define DY_LIGHT_INLINE inline
#include "LightingMath.inc"

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
        float4 metallicRoughness = MetallicRoughnessTexture.Sample(LinearSampler, input.uv);
        roughness = clamp(roughness * metallicRoughness.g, minRoughness, 1.0);
        metallic = clamp(metallic * metallicRoughness.b, 0.0, 1.0);
    }
    if ((textureFlags & RENDERER_TEXTURE_FLAG_OCCLUSION) != 0u)
    {
        occlusion *= OcclusionTexture.Sample(LinearSampler, input.uv).r;
    }
    float3 emissive = emissiveColor.rgb;
    if ((textureFlags & RENDERER_TEXTURE_FLAG_EMISSIVE) != 0u)
    {
        emissive *= EmissiveTexture.Sample(LinearSampler, input.uv).rgb;
    }

    float3 normal = GetNormal(input, textureFlags);
    float3 viewDir = normalize(lighting.cameraPosition.xyz - input.worldPosition);
    bool usePointLight = lighting.pointLightColorIntensity.a > 0.0 && lighting.pointLightPositionRange.w > 0.0;
    float3 lightDir = normalize(lighting.directionalLightDirection.xyz);
    float3 radiance = lighting.directionalLightColor.rgb * lighting.directionalLightColor.a;
    if (usePointLight)
    {
        float3 toLight = lighting.pointLightPositionRange.xyz - input.worldPosition;
        float distanceToLight = length(toLight);
        lightDir = distanceToLight > 0.0001 ? toLight / distanceToLight : float3(0.0, 0.0, 1.0);
        float range = max(lighting.pointLightPositionRange.w, 0.0001);
        float rangeFade = saturate(1.0 - distanceToLight / range);
        rangeFade *= rangeFade;
        float attenuation = rangeFade / max(1.0 + 0.18 * distanceToLight + 0.06 * distanceToLight * distanceToLight, 0.0001);
        radiance = lighting.pointLightColorIntensity.rgb * lighting.pointLightColorIntensity.a * attenuation;
    }

    float3 halfway = normalize(viewDir + lightDir);
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float ndf = DistributionGGX(normal, halfway, roughness);
    float geometry = GeometrySmith(normal, viewDir, lightDir, roughness);
    float3 fresnel = FresnelSchlick(max(dot(halfway, viewDir), 0.0), f0);
    float3 specular = (ndf * geometry * fresnel) /
        max(4.0 * max(dot(normal, viewDir), 0.0) * max(dot(normal, lightDir), 0.0), 0.0001);

    float3 kS = fresnel;
    float3 kD = (float3(1.0, 1.0, 1.0) - kS) * (1.0 - metallic);
    float ndotl = max(dot(normal, lightDir), 0.0);
#if RENDERER_ENABLE_SHADOWS
    float shadowVisibility = CalculateShadowVisibility(input.lightSpacePosition, normal, lightDir);
#else
    float shadowVisibility = 1.0;
#endif
    float3 directLight = EvaluateLights(lighting, input.worldPosition, normal, viewDir, albedo, metallic, roughness, shadowVisibility);
    float3 ambientFresnel = FresnelSchlickRoughness(max(dot(normal, viewDir), 0.0), f0, roughness);
    float3 ambientDiffuse = kD * albedo * lighting.ambientColor.rgb * lighting.ambientColor.a;
    float3 ambientSpecular = ambientFresnel * lighting.environmentColor.rgb * lighting.environmentColor.a * ambientSpecularStrength;
    float3 ambient = (ambientDiffuse + ambientSpecular) * occlusion;
    float3 color = ambient + directLight + emissive;

    color = color / (color + float3(1.0, 1.0, 1.0));
    if (lighting.pbrParams.z > 0.5) color = pow(color, float3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2));
    return float4(color, baseColor.a);
}
