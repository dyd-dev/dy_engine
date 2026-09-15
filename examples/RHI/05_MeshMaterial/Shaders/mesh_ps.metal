#include <metal_stdlib>
using namespace metal;
struct DrawConstants
{
    float4x4 viewProjectionMatrix;
    float4x4 modelMatrix;
    float4 baseColor;
    float metallic;
    float roughness;
    uint useBaseColorTexture;
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
    float3 worldPosition [[user(locn0)]];
    float3 worldNormal [[user(locn1)]];
    float2 uv [[user(locn2)]];
};

constant float PI = 3.14159265359f;

float DistributionGGX(float3 normal, float3 halfway, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float ndoth = max(dot(normal, halfway), 0.0f);
    float ndoth2 = ndoth * ndoth;
    float denominator = ndoth2 * (a2 - 1.0f) + 1.0f;
    return a2 / max(PI * denominator * denominator, 0.0001f);
}

float GeometrySchlickGGX(float ndotv, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    return ndotv / max(ndotv * (1.0f - k) + k, 0.0001f);
}

float GeometrySmith(float3 normal, float3 viewDir, float3 lightDir, float roughness)
{
    return GeometrySchlickGGX(max(dot(normal, viewDir), 0.0f), roughness)
        * GeometrySchlickGGX(max(dot(normal, lightDir), 0.0f), roughness);
}

float3 FresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0f - f0) * pow(clamp(1.0f - cosTheta, 0.0f, 1.0f), 5.0f);
}

float3 FresnelSchlickRoughness(float cosTheta, float3 f0, float roughness)
{
    float3 roughnessF0 = max(float3(1.0f - roughness), f0);
    return f0 + (roughnessF0 - f0) * pow(clamp(1.0f - cosTheta, 0.0f, 1.0f), 5.0f);
}

// 원래 PBR 수식으로 방향광 하나를 평가한다.
float3 EvaluateDirectLight(float3 normal, float3 viewDir, float3 lightDir,
    float3 incidentIlluminance, float3 albedo, float metallic, float roughness)
{
    float ndotl = max(dot(normal, lightDir), 0.0f);
    float ndotv = max(dot(normal, viewDir), 0.0f);
    if(ndotl <= 0.0f || ndotv <= 0.0f) return float3(0.0f);
    float3 halfwayVector = viewDir + lightDir;
    float halfwayLengthSquared = dot(halfwayVector, halfwayVector);
    if(halfwayLengthSquared <= 0.000001f) return float3(0.0f);
    float3 halfway = halfwayVector * rsqrt(halfwayLengthSquared);
    float3 f0 = mix(float3(0.04f), albedo, metallic);
    float ndf = DistributionGGX(normal, halfway, roughness);
    float geometry = GeometrySmith(normal, viewDir, lightDir, roughness);
    float3 fresnel = FresnelSchlick(max(dot(halfway, viewDir), 0.0f), f0);
    float3 specular = (ndf * geometry * fresnel) / max(4.0f * ndotv * ndotl, 0.0001f);
    float3 diffuseWeight = (float3(1.0f) - fresnel) * (1.0f - metallic);
    return (diffuseWeight * albedo / PI + specular) * incidentIlluminance * ndotl;
}

fragment float4 fragmentMain(RasterData input [[stage_in]],
    constant DrawConstants& draw [[buffer(10)]],
    constant LightingConstants& lighting [[buffer(1)]],
    texture2d<float> baseColorTexture [[texture(0)]],
    sampler materialSampler [[sampler(8)]])
{
    float3 albedo = draw.baseColor.rgb;
    if(draw.useBaseColorTexture != 0u) albedo *= baseColorTexture.sample(materialSampler, input.uv).rgb;
    float minRoughness = clamp(lighting.pbrParams.x, 0.01f, 1.0f);
    float metallic = clamp(draw.metallic, 0.0f, 1.0f);
    float roughness = clamp(draw.roughness, minRoughness, 1.0f);
    float3 normal = normalize(input.worldNormal);
    float3 viewDir = normalize(lighting.cameraPosition.xyz - input.worldPosition);
    float3 lightDir = normalize(lighting.directionalLightDirection.xyz);
    float3 halfway = normalize(viewDir + lightDir);
    float3 f0 = mix(float3(0.04f), albedo, metallic);
    float3 fresnel = FresnelSchlick(max(dot(halfway, viewDir), 0.0f), f0);
    float3 diffuseWeight = (float3(1.0f) - fresnel) * (1.0f - metallic);
    float3 directLight = EvaluateDirectLight(normal, viewDir, lightDir,
        lighting.directionalLightColor.rgb * lighting.directionalLightColor.a,
        albedo, metallic, roughness);
    float3 ambientFresnel = FresnelSchlickRoughness(max(dot(normal, viewDir), 0.0f), f0, roughness);
    float3 ambientDiffuse = diffuseWeight * albedo * lighting.ambientColor.rgb * lighting.ambientColor.a;
    float3 ambientSpecular = ambientFresnel * lighting.environmentColor.rgb * lighting.environmentColor.a
        * max(lighting.pbrParams.y, 0.0f);
    float3 color = ambientDiffuse + ambientSpecular + directLight;
    color = color / (color + float3(1.0f));
    if(lighting.pbrParams.z > 0.5f) color = pow(color, float3(1.0f / 2.2f));
    return float4(color, draw.baseColor.a);
}
