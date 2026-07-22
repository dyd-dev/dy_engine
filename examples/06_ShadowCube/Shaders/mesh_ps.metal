#include <metal_stdlib>

using namespace metal;

struct DrawConstants
{
    float4x4 viewProjectionMatrix;
    float4x4 modelMatrix;
    float drawMode;
    uint instanceTransformOffset;
    float4 emissiveColor;
    float4 baseColor;
    float4 materialParams;
    float4 textureIndices;
};

struct RendererLighting
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
};

struct RasterData
{
    float4 position [[position]];
    float2 uv [[user(locn0)]];
    float3 worldPosition [[user(locn1)]];
    float3 worldNormal [[user(locn2)]];
    float4 worldTangent [[user(locn3)]];
    float4 lightSpacePosition [[user(locn4)]];
};

constant uint kTextureFlagBaseColor = 1u;
constant uint kTextureFlagMetallicRoughness = 2u;
constant uint kTextureFlagNormal = 4u;
constant uint kTextureFlagOcclusion = 8u;
constant uint kTextureFlagEmissive = 16u;
constant uint kTextureFlagReceiveShadow = 32u;
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

inline float3 ResolveNormal(
    RasterData input,
    constant DrawConstants& drawConstants,
    texture2d<float> normalTexture,
    sampler materialSampler)
{
    float3 normal = normalize(input.worldNormal);
    const uint textureFlags = uint(drawConstants.drawMode + 0.5f);
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

inline float CalculateShadowVisibility(
    float4 lightSpacePosition,
    float3 normal,
    float3 lightDirection,
    uint textureFlags,
    constant RendererLighting& lighting,
    depth2d<float> shadowMap,
    sampler shadowSampler)
{
    if (lighting.directionalLightDirection.w < 0.5f ||
        (textureFlags & kTextureFlagReceiveShadow) == 0u)
    {
        return 1.0f;
    }

    const float3 ndc = lightSpacePosition.xyz / lightSpacePosition.w;
    const float2 shadowUv = ndc.xy * float2(0.5f, -0.5f) + 0.5f;
    if (shadowUv.x < 0.0f || shadowUv.x > 1.0f ||
        shadowUv.y < 0.0f || shadowUv.y > 1.0f ||
        ndc.z < 0.0f || ndc.z > 1.0f)
    {
        return 1.0f;
    }

    const float ndotl = max(dot(normal, lightDirection), 0.0f);
    const float constantBias = max(lighting.shadowParams.x, 0.0f);
    const float slopeBias =
        max(lighting.shadowParams.y, 0.0f) * (1.0f - ndotl);
    const float normalBias =
        max(lighting.shadowParams.z, 0.0f) * (1.0f - ndotl);
    const float bias = max(slopeBias, constantBias) + normalBias;

    const float2 texelSize = 1.0f / float2(
        max(shadowMap.get_width(), 1u),
        max(shadowMap.get_height(), 1u));
    const int pcfRadius =
        clamp(int(lighting.shadowParams.w + 0.5f), 0, 4);
    float visibility = 0.0f;
    int sampleCount = 0;
    for (int y = -pcfRadius; y <= pcfRadius; ++y)
    {
        for (int x = -pcfRadius; x <= pcfRadius; ++x)
        {
            const float sampledDepth = shadowMap.sample(
                shadowSampler,
                shadowUv + float2(x, y) * texelSize);
            visibility +=
                (ndc.z - bias) > sampledDepth ? 0.0f : 1.0f;
            ++sampleCount;
        }
    }

    visibility /= max(float(sampleCount), 1.0f);
    return mix(
        1.0f,
        visibility,
        saturate(lighting.cameraPosition.w));
}

inline float4 RunFragmentShader(
    RasterData input,
    constant DrawConstants& drawConstants,
    constant RendererLighting& lighting,
    texture2d<float> baseColorTexture,
    depth2d<float> shadowMap,
    texture2d<float> metallicRoughnessTexture,
    texture2d<float> normalTexture,
    texture2d<float> occlusionTexture,
    texture2d<float> emissiveTexture,
    sampler materialSampler,
    sampler shadowSampler)
{
    const uint textureFlags = uint(drawConstants.drawMode + 0.5f);
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
    float3 radiance =
        lighting.directionalLightColor.rgb *
        lighting.directionalLightColor.a;

    if (usePointLight)
    {
        const float3 toLight =
            lighting.pointLightPositionRange.xyz - input.worldPosition;
        const float distanceToLight = length(toLight);
        lightDirection = distanceToLight > 0.0001f
            ? toLight / distanceToLight
            : float3(0.0f, 0.0f, 1.0f);
        const float range =
            max(lighting.pointLightPositionRange.w, 0.0001f);
        float rangeFade =
            saturate(1.0f - distanceToLight / range);
        rangeFade *= rangeFade;
        const float attenuation = rangeFade / max(
            1.0f +
                0.18f * distanceToLight +
                0.06f * distanceToLight * distanceToLight,
            0.0001f);
        radiance =
            lighting.pointLightColorIntensity.rgb *
            lighting.pointLightColorIntensity.a *
            attenuation;
    }

    const float3 halfway =
        normalize(viewDirection + lightDirection);
    const float3 f0 =
        mix(float3(0.04f), albedo, metallic);
    const float distribution =
        DistributionGGX(normal, halfway, roughness);
    const float geometry =
        GeometrySmith(normal, viewDirection, lightDirection, roughness);
    const float3 fresnel =
        FresnelSchlick(max(dot(halfway, viewDirection), 0.0f), f0);
    const float3 specular =
        distribution * geometry * fresnel /
        max(
            4.0f *
                max(dot(normal, viewDirection), 0.0f) *
                max(dot(normal, lightDirection), 0.0f),
            0.0001f);

    const float3 kS = fresnel;
    const float3 kD =
        (float3(1.0f) - kS) * (1.0f - metallic);
    const float ndotl =
        max(dot(normal, lightDirection), 0.0f);
    const float shadowVisibility = CalculateShadowVisibility(
        input.lightSpacePosition,
        normal,
        lightDirection,
        textureFlags,
        lighting,
        shadowMap,
        shadowSampler);
    const float3 directLight =
        (kD * albedo / kPi + specular) *
        radiance *
        ndotl *
        shadowVisibility;

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
    color = pow(color, float3(1.0f / 2.2f));
    return float4(color, drawConstants.baseColor.a);
}

fragment float4 fragmentShader(
    RasterData input [[stage_in]],
    constant DrawConstants& drawConstants [[buffer(0)]],
    constant RendererLighting& lighting [[buffer(1)]],
    texture2d<float> baseColorTexture [[texture(0)]],
    depth2d<float> shadowMap [[texture(2)]],
    texture2d<float> metallicRoughnessTexture [[texture(6)]],
    texture2d<float> normalTexture [[texture(7)]],
    texture2d<float> occlusionTexture [[texture(8)]],
    texture2d<float> emissiveTexture [[texture(9)]],
    sampler materialSampler [[sampler(0)]],
    sampler shadowSampler [[sampler(2)]])
{
    return RunFragmentShader(
        input,
        drawConstants,
        lighting,
        baseColorTexture,
        shadowMap,
        metallicRoughnessTexture,
        normalTexture,
        occlusionTexture,
        emissiveTexture,
        materialSampler,
        shadowSampler);
}

// Stock Renderer가 ShaderDesc에서 명시적으로 선택하는 entry point.
fragment float4 main0(
    RasterData input [[stage_in]],
    constant DrawConstants& drawConstants [[buffer(0)]],
    constant RendererLighting& lighting [[buffer(1)]],
    texture2d<float> baseColorTexture [[texture(0)]],
    depth2d<float> shadowMap [[texture(2)]],
    texture2d<float> metallicRoughnessTexture [[texture(6)]],
    texture2d<float> normalTexture [[texture(7)]],
    texture2d<float> occlusionTexture [[texture(8)]],
    texture2d<float> emissiveTexture [[texture(9)]],
    sampler materialSampler [[sampler(0)]],
    sampler shadowSampler [[sampler(2)]])
{
    return RunFragmentShader(
        input,
        drawConstants,
        lighting,
        baseColorTexture,
        shadowMap,
        metallicRoughnessTexture,
        normalTexture,
        occlusionTexture,
        emissiveTexture,
        materialSampler,
        shadowSampler);
}
