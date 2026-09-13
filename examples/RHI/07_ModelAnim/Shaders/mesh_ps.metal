#include <metal_stdlib>

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

struct LightingConstants
{
    float4 cameraPosition;
    float4 directionalLightDirection;
    float4 directionalLightColor;
    float4 ambientColor;
    float4 pbrParams;
    float4 environmentColor;
};

struct RasterData
{
    float4 position [[position]];
    float2 uv [[user(locn0)]];
    float3 worldPosition [[user(locn1)]];
    float3 worldNormal [[user(locn2)]];
    float4 worldTangent [[user(locn3)]];
};

constant uint kTextureFlagBaseColor =
    uint(1);
constant uint kTextureFlagMetallicRoughness =
    uint(2);
constant uint kTextureFlagNormal =
    uint(4);
constant uint kTextureFlagOcclusion =
    uint(8);
constant uint kTextureFlagEmissive =
    uint(16);
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

inline float3 EvaluateDirectLight(
    float3 normal,
    float3 viewDir,
    float3 lightDir,
    float3 incidentIlluminance,
    float3 albedo,
    float metallic,
    float roughness) {
    float ndotl = max(dot(normal, lightDir), 0.0);
    float ndotv = max(dot(normal, viewDir), 0.0);
    if (ndotl <= 0.0 || ndotv <= 0.0) {
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
    return (diffuseWeight * albedo / kPi + specular) * incidentIlluminance * ndotl;
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
    constant LightingConstants& lighting,
    texture2d<float> baseColorTexture,
    texture2d<float> metallicRoughnessTexture,
    texture2d<float> normalTexture,
    texture2d<float> occlusionTexture,
    texture2d<float> emissiveTexture,
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

    float3 lightDirection =
        normalize(lighting.directionalLightDirection.xyz);

    const float3 halfway =
        normalize(viewDirection + lightDirection);
    const float3 f0 =
        mix(float3(0.04f), albedo, metallic);
    const float3 fresnel =
        FresnelSchlick(max(dot(halfway, viewDirection), 0.0f), f0);

    const float3 kS = fresnel;
    const float3 kD =
        (float3(1.0f) - kS) * (1.0f - metallic);
    const float3 directLight = EvaluateDirectLight(normal, viewDirection, lightDirection,
        lighting.directionalLightColor.rgb * max(lighting.directionalLightColor.a, 0.0),
        albedo, metallic, roughness);

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

    float3 color = ambient + directLight + emissive;
    color = color / (color + 1.0f);
    if (lighting.pbrParams.z > 0.5f) color = pow(color, float3(1.0f / 2.2f));
    return float4(color, drawConstants.baseColor.a);
}

fragment float4 fragmentMain(
    RasterData input [[stage_in]],
    constant DrawConstants& drawConstants [[buffer(10)]],
    constant LightingConstants& lighting [[buffer(1)]],
    texture2d<float> baseColorTexture [[texture(0)]],
    texture2d<float> metallicRoughnessTexture [[texture(4)]],
    texture2d<float> normalTexture [[texture(5)]],
    texture2d<float> occlusionTexture [[texture(6)]],
    texture2d<float> emissiveTexture [[texture(7)]],
    sampler materialSampler [[sampler(8)]])
{
    return RunFragmentShader(
        input,
        drawConstants,
        lighting,
        baseColorTexture,
        metallicRoughnessTexture,
        normalTexture,
        occlusionTexture,
        emissiveTexture,
        materialSampler);
}
