#include <metal_stdlib>
using namespace metal;

constant uint TextureFlagBaseColor = 1u;
constant uint TextureFlagMetallicRoughness = 2u;
constant uint TextureFlagNormal = 4u;
constant uint TextureFlagOcclusion = 8u;
constant uint TextureFlagEmissive = 16u;
constant float Pi = 3.14159265359f;

struct DrawConstants
{
    float4x4 viewProjectionMatrix;
    float4x4 modelMatrix;
    float drawMode;
    uint firstIndex;
    int vertexOffset;
    uint firstVertex;
    float4 emissiveColor;
    float4 baseColor;
    float4 materialParams;
    float4 textureIndices;
};

struct DirectionalLight
{
    float4 directionIntensity;
    float4 color;
};

struct PointLight
{
    float4 positionRange;
    float4 colorIntensity;
};

struct SpotLight
{
    float4 positionRange;
    float4 directionOuterCos;
    float4 colorIntensity;
    float4 coneParams;
};

struct RectAreaLight
{
    float4 positionIntensity;
    float4 directionWidth;
    float4 upHeight;
    float4 color;
};

struct DiscAreaLight
{
    float4 positionIntensity;
    float4 directionRadius;
    float4 up;
    float4 color;
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
    float4 lightCounts;
    float4 areaLightCounts;
    DirectionalLight directionalLights[4];
    PointLight pointLights[16];
    SpotLight spotLights[16];
    RectAreaLight rectAreaLights[4];
    DiscAreaLight discAreaLights[4];
    float4 shadowLight;
};

struct FragmentInput
{
    float4 position [[position]];
    float2 uv [[user(locn0)]];
    float3 worldPosition [[user(locn1)]];
    float3 worldNormal [[user(locn2)]];
    float4 worldTangent [[user(locn3)]];
};

float DistributionGGX(float3 normal, float3 halfway, float roughness)
{
    const float a = roughness * roughness;
    const float a2 = a * a;
    const float normalDotHalfway = max(dot(normal, halfway), 0.0f);
    const float normalDotHalfway2 = normalDotHalfway * normalDotHalfway;
    const float denominator = normalDotHalfway2 * (a2 - 1.0f) + 1.0f;
    return a2 / max(Pi * denominator * denominator, 0.0001f);
}

float GeometrySchlickGGX(float normalDotView, float roughness)
{
    const float r = roughness + 1.0f;
    const float k = (r * r) / 8.0f;
    return normalDotView / max(normalDotView * (1.0f - k) + k, 0.0001f);
}

float GeometrySmith(float3 normal, float3 viewDirection, float3 lightDirection, float roughness)
{
    return GeometrySchlickGGX(max(dot(normal, viewDirection), 0.0f), roughness)
        * GeometrySchlickGGX(max(dot(normal, lightDirection), 0.0f), roughness);
}

float3 FresnelSchlick(float cosine, float3 f0)
{
    return f0 + (1.0f - f0) * pow(saturate(1.0f - cosine), 5.0f);
}

float3 FresnelSchlickRoughness(float cosine, float3 f0, float roughness)
{
    const float3 roughnessF0 = max(float3(1.0f - roughness), f0);
    return f0 + (roughnessF0 - f0) * pow(saturate(1.0f - cosine), 5.0f);
}

float PunctualAttenuation(float distanceToLight, float range)
{
    if(range <= 0.0f || distanceToLight >= range) return 0.0f;
    const float distanceSquared = max(distanceToLight * distanceToLight, 0.0001f);
    const float ratio = distanceToLight / range;
    const float ratioSquared = ratio * ratio;
    const float window = max(1.0f - ratioSquared * ratioSquared, 0.0f);
    return (window * window) / distanceSquared;
}

float SpotConeAttenuation(float cosine, float innerCosine, float outerCosine)
{
    const float denominator = max(innerCosine - outerCosine, 0.0001f);
    const float t = saturate((cosine - outerCosine) / denominator);
    return t * t * (3.0f - 2.0f * t);
}

float3 EvaluateDirectLight(
    float3 normal,
    float3 viewDirection,
    float3 lightDirection,
    float3 incidentIlluminance,
    float3 albedo,
    float metallic,
    float roughness)
{
    const float normalDotLight = max(dot(normal, lightDirection), 0.0f);
    const float normalDotView = max(dot(normal, viewDirection), 0.0f);
    if(normalDotLight <= 0.0f || normalDotView <= 0.0f) return float3(0.0f);

    const float3 halfwayVector = viewDirection + lightDirection;
    const float halfwayLengthSquared = dot(halfwayVector, halfwayVector);
    if(halfwayLengthSquared <= 0.000001f) return float3(0.0f);
    const float3 halfway = halfwayVector * rsqrt(halfwayLengthSquared);
    const float3 f0 = mix(float3(0.04f), albedo, metallic);
    const float distribution = DistributionGGX(normal, halfway, roughness);
    const float geometry = GeometrySmith(normal, viewDirection, lightDirection, roughness);
    const float3 fresnel = FresnelSchlick(max(dot(halfway, viewDirection), 0.0f), f0);
    const float3 specular = distribution * geometry * fresnel
        / max(4.0f * normalDotView * normalDotLight, 0.0001f);
    const float3 diffuseWeight = (1.0f - fresnel) * (1.0f - metallic);
    return (diffuseWeight * albedo / Pi + specular) * incidentIlluminance * normalDotLight;
}

float3 EvaluateAreaSample(
    float3 worldPosition,
    float3 normal,
    float3 viewDirection,
    float3 samplePosition,
    float3 emitterDirection,
    float3 emittedRadiance,
    float sampleArea,
    float3 albedo,
    float metallic,
    float roughness)
{
    const float3 toLight = samplePosition - worldPosition;
    const float distanceSquared = dot(toLight, toLight);
    if(distanceSquared <= 0.000001f || sampleArea <= 0.0f) return float3(0.0f);
    const float3 lightDirection = toLight * rsqrt(distanceSquared);
    const float emitterCosine = max(dot(emitterDirection, -lightDirection), 0.0f);
    const float3 illuminance = emittedRadiance * emitterCosine * sampleArea / distanceSquared;
    return EvaluateDirectLight(normal, viewDirection, lightDirection, illuminance, albedo, metallic, roughness);
}

float3 EvaluateRectAreaLight(
    RectAreaLight light,
    float3 worldPosition,
    float3 normal,
    float3 viewDirection,
    float3 albedo,
    float metallic,
    float roughness)
{
    const float3 emitterDirection = normalize(light.directionWidth.xyz);
    float3 emitterUp = normalize(light.upHeight.xyz);
    const float3 emitterRight = normalize(cross(emitterDirection, emitterUp));
    emitterUp = normalize(cross(emitterRight, emitterDirection));
    const float width = max(light.directionWidth.w, 0.0f);
    const float height = max(light.upHeight.w, 0.0f);
    const float sampleArea = width * height * 0.25f;
    const float3 emittedRadiance = light.color.rgb * max(light.positionIntensity.w, 0.0f);
    float3 result = float3(0.0f);
    for(int sampleIndex = 0; sampleIndex < 4; ++sampleIndex)
    {
        const float x = (sampleIndex & 1) == 0 ? -0.25f : 0.25f;
        const float y = (sampleIndex & 2) == 0 ? -0.25f : 0.25f;
        const float3 samplePosition = light.positionIntensity.xyz
            + emitterRight * (x * width) + emitterUp * (y * height);
        result += EvaluateAreaSample(worldPosition, normal, viewDirection, samplePosition,
            emitterDirection, emittedRadiance, sampleArea, albedo, metallic, roughness);
    }
    return result;
}

float3 EvaluateDiscAreaLight(
    DiscAreaLight light,
    float3 worldPosition,
    float3 normal,
    float3 viewDirection,
    float3 albedo,
    float metallic,
    float roughness)
{
    constexpr int sampleCount = 8;
    constexpr float goldenAngle = 2.39996323f;
    const float3 emitterDirection = normalize(light.directionRadius.xyz);
    float3 emitterUp = normalize(light.up.xyz);
    const float3 emitterRight = normalize(cross(emitterDirection, emitterUp));
    emitterUp = normalize(cross(emitterRight, emitterDirection));
    const float radius = max(light.directionRadius.w, 0.0f);
    const float sampleArea = Pi * radius * radius / float(sampleCount);
    const float3 emittedRadiance = light.color.rgb * max(light.positionIntensity.w, 0.0f);
    float3 result = float3(0.0f);
    for(int sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
    {
        const float normalizedRadius = sqrt((float(sampleIndex) + 0.5f) / float(sampleCount));
        const float angle = float(sampleIndex) * goldenAngle;
        const float2 discOffset = float2(cos(angle), sin(angle)) * (normalizedRadius * radius);
        const float3 samplePosition = light.positionIntensity.xyz
            + emitterRight * discOffset.x + emitterUp * discOffset.y;
        result += EvaluateAreaSample(worldPosition, normal, viewDirection, samplePosition,
            emitterDirection, emittedRadiance, sampleArea, albedo, metallic, roughness);
    }
    return result;
}

float3 EnvironmentBrdfApproximation(float3 f0, float roughness, float normalDotView)
{
    const float4 c0 = float4(-1.0f, -0.0275f, -0.572f, 0.022f);
    const float4 c1 = float4(1.0f, 0.0425f, 1.04f, -0.04f);
    const float4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28f * normalDotView)) * r.x + r.y;
    const float2 scaleBias = float2(-1.04f, 1.04f) * a004 + r.zw;
    return f0 * scaleBias.x + scaleBias.y;
}

float3 AcesFitted(float3 color)
{
    constexpr float a = 2.51f;
    constexpr float b = 0.03f;
    constexpr float c = 2.43f;
    constexpr float d = 0.59f;
    constexpr float e = 0.14f;
    return saturate((color * (a * color + b)) / (color * (c * color + d) + e));
}

fragment float4 main0(
    FragmentInput input [[stage_in]],
    constant DrawConstants& draw [[buffer(0)]],
    constant RendererLighting& lighting [[buffer(1)]],
    texture2d<float> baseColorTexture [[texture(0)]],
    texture2d<float> metallicRoughnessTexture [[texture(6)]],
    texture2d<float> normalTexture [[texture(7)]],
    texture2d<float> occlusionTexture [[texture(8)]],
    texture2d<float> emissiveTexture [[texture(9)]])
{
    constexpr sampler linearSampler(filter::linear, mip_filter::linear, address::repeat);
    const uint textureFlags = uint(draw.drawMode + 0.5f);

    float3 albedo = draw.baseColor.rgb;
    if((textureFlags & TextureFlagBaseColor) != 0u)
        albedo *= baseColorTexture.sample(linearSampler, input.uv).rgb;

    const float minRoughness = clamp(lighting.pbrParams.x, 0.01f, 1.0f);
    const float ambientSpecularStrength = max(lighting.pbrParams.y, 0.0f);
    float metallic = clamp(draw.materialParams.x, 0.0f, 1.0f);
    float roughness = clamp(draw.materialParams.y, minRoughness, 1.0f);
    float occlusion = clamp(draw.materialParams.w, 0.0f, 1.0f);
    if((textureFlags & TextureFlagMetallicRoughness) != 0u)
    {
        const float4 sample = metallicRoughnessTexture.sample(linearSampler, input.uv);
        roughness = clamp(roughness * sample.g, minRoughness, 1.0f);
        metallic = clamp(metallic * sample.b, 0.0f, 1.0f);
    }
    if((textureFlags & TextureFlagOcclusion) != 0u)
        occlusion *= occlusionTexture.sample(linearSampler, input.uv).r;

    float3 normal = normalize(input.worldNormal);
    if((textureFlags & TextureFlagNormal) != 0u && draw.materialParams.z > 0.0001f)
    {
        const float3 tangent = normalize(input.worldTangent.xyz
            - normal * dot(normal, input.worldTangent.xyz));
        const float3 bitangent = normalize(cross(normal, tangent)) * input.worldTangent.w;
        float3 tangentNormal = normalTexture.sample(linearSampler, input.uv).xyz * 2.0f - 1.0f;
        tangentNormal.xy *= draw.materialParams.z;
        normal = normalize(float3x3(tangent, bitangent, normal) * tangentNormal);
    }

    const float3 viewDirection = normalize(lighting.cameraPosition.xyz - input.worldPosition);
    float3 directLight = float3(0.0f);

    const int directionalCount = clamp(int(lighting.lightCounts.x + 0.5f), 0, 4);
    for(int lightIndex = 0; lightIndex < directionalCount; ++lightIndex)
    {
        const DirectionalLight light = lighting.directionalLights[lightIndex];
        directLight += EvaluateDirectLight(normal, viewDirection,
            normalize(light.directionIntensity.xyz),
            light.color.rgb * max(light.directionIntensity.w, 0.0f),
            albedo, metallic, roughness);
    }

    const int pointCount = clamp(int(lighting.lightCounts.y + 0.5f), 0, 16);
    for(int lightIndex = 0; lightIndex < pointCount; ++lightIndex)
    {
        const PointLight light = lighting.pointLights[lightIndex];
        const float3 toLight = light.positionRange.xyz - input.worldPosition;
        const float distanceToLight = length(toLight);
        if(distanceToLight <= 0.0001f) continue;
        const float3 lightDirection = toLight / distanceToLight;
        const float3 illuminance = light.colorIntensity.rgb * max(light.colorIntensity.w, 0.0f)
            * PunctualAttenuation(distanceToLight, light.positionRange.w);
        directLight += EvaluateDirectLight(normal, viewDirection, lightDirection,
            illuminance, albedo, metallic, roughness);
    }

    const int spotCount = clamp(int(lighting.lightCounts.z + 0.5f), 0, 16);
    for(int lightIndex = 0; lightIndex < spotCount; ++lightIndex)
    {
        const SpotLight light = lighting.spotLights[lightIndex];
        const float3 toLight = light.positionRange.xyz - input.worldPosition;
        const float distanceToLight = length(toLight);
        if(distanceToLight <= 0.0001f) continue;
        const float3 lightDirection = toLight / distanceToLight;
        const float attenuation = PunctualAttenuation(distanceToLight, light.positionRange.w)
            * SpotConeAttenuation(dot(-lightDirection, normalize(light.directionOuterCos.xyz)),
                light.coneParams.x, light.directionOuterCos.w);
        const float3 illuminance = light.colorIntensity.rgb * max(light.colorIntensity.w, 0.0f) * attenuation;
        directLight += EvaluateDirectLight(normal, viewDirection, lightDirection,
            illuminance, albedo, metallic, roughness);
    }

    const int rectAreaCount = clamp(int(lighting.areaLightCounts.x + 0.5f), 0, 4);
    for(int lightIndex = 0; lightIndex < rectAreaCount; ++lightIndex)
        directLight += EvaluateRectAreaLight(lighting.rectAreaLights[lightIndex], input.worldPosition,
            normal, viewDirection, albedo, metallic, roughness);

    const int discAreaCount = clamp(int(lighting.areaLightCounts.y + 0.5f), 0, 4);
    for(int lightIndex = 0; lightIndex < discAreaCount; ++lightIndex)
        directLight += EvaluateDiscAreaLight(lighting.discAreaLights[lightIndex], input.worldPosition,
            normal, viewDirection, albedo, metallic, roughness);

    const float3 f0 = mix(float3(0.04f), albedo, metallic);
    const float normalDotView = max(dot(normal, viewDirection), 0.0f);
    const float3 ambientFresnel = FresnelSchlickRoughness(normalDotView, f0, roughness);
    const float3 ambientDiffuseWeight = (1.0f - ambientFresnel) * (1.0f - metallic);
    const float3 ambientDiffuse = ambientDiffuseWeight * albedo
        * lighting.ambientColor.rgb * lighting.ambientColor.a;
    const float3 environmentBrdf = EnvironmentBrdfApproximation(f0, roughness, normalDotView);
    const float3 ambientSpecular = environmentBrdf * lighting.environmentColor.rgb
        * lighting.environmentColor.a * ambientSpecularStrength;

    float3 emissive = draw.emissiveColor.rgb;
    if((textureFlags & TextureFlagEmissive) != 0u)
        emissive *= emissiveTexture.sample(linearSampler, input.uv).rgb;

    float3 color = (ambientDiffuse + ambientSpecular) * occlusion + directLight + emissive;
    if(lighting.pbrParams.w > 0.5f) color = AcesFitted(max(color, float3(0.0f)));
    if(lighting.pbrParams.z > 0.5f) color = pow(max(color, float3(0.0f)), float3(1.0f / 2.2f));
    return float4(color, draw.baseColor.a);
}
