cbuffer DrawConstants : register(b10, space0)
{
    column_major float4x4 viewProjectionMatrix;
    column_major float4x4 modelMatrix;
    float4 baseColor;
    float drawMetallic;
    float drawRoughness;
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
cbuffer LightingBuffer : register(b1, space0) { LightingConstants lighting; };

struct RasterData
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
};

static const float PI = 3.14159265359;

float DistributionGGX(float3 normal, float3 halfway, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float ndoth = max(dot(normal, halfway), 0.0);
    float ndoth2 = ndoth * ndoth;
    float denominator = ndoth2 * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denominator * denominator, 0.0001);
}

float GeometrySchlickGGX(float ndotv, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return ndotv / max(ndotv * (1.0 - k) + k, 0.0001);
}

float GeometrySmith(float3 normal, float3 viewDir, float3 lightDir, float roughness)
{
    return GeometrySchlickGGX(max(dot(normal, viewDir), 0.0), roughness)
        * GeometrySchlickGGX(max(dot(normal, lightDir), 0.0), roughness);
}

float3 FresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

float3 FresnelSchlickRoughness(float cosTheta, float3 f0, float roughness)
{
    float3 roughnessF0 = max(float3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness), f0);
    return f0 + (roughnessF0 - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// 원래 PBR 수식으로 방향광 하나를 평가한다.
float3 EvaluateDirectLight(float3 normal, float3 viewDir, float3 lightDir,
    float3 incidentIlluminance, float3 albedo, float metallic, float roughness)
{
    float ndotl = max(dot(normal, lightDir), 0.0);
    float ndotv = max(dot(normal, viewDir), 0.0);
    if(ndotl <= 0.0 || ndotv <= 0.0) return float3(0.0, 0.0, 0.0);
    float3 halfwayVector = viewDir + lightDir;
    float halfwayLengthSquared = dot(halfwayVector, halfwayVector);
    if(halfwayLengthSquared <= 0.000001) return float3(0.0, 0.0, 0.0);
    float3 halfway = halfwayVector * rsqrt(halfwayLengthSquared);
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float ndf = DistributionGGX(normal, halfway, roughness);
    float geometry = GeometrySmith(normal, viewDir, lightDir, roughness);
    float3 fresnel = FresnelSchlick(max(dot(halfway, viewDir), 0.0), f0);
    float3 specular = (ndf * geometry * fresnel) / max(4.0 * ndotv * ndotl, 0.0001);
    float3 diffuseWeight = (float3(1.0, 1.0, 1.0) - fresnel) * (1.0 - metallic);
    return (diffuseWeight * albedo / PI + specular) * incidentIlluminance * ndotl;
}

float4 main(RasterData input) : SV_TARGET
{
    float3 albedo = baseColor.rgb;

    float minRoughness = clamp(lighting.pbrParams.x, 0.01, 1.0);
    float metallic = clamp(drawMetallic, 0.0, 1.0);
    float roughness = clamp(drawRoughness, minRoughness, 1.0);
    float3 normal = normalize(input.worldNormal);
    float3 viewDir = normalize(lighting.cameraPosition.xyz - input.worldPosition);
    float3 lightDir = normalize(lighting.directionalLightDirection.xyz);
    float3 halfway = normalize(viewDir + lightDir);
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 fresnel = FresnelSchlick(max(dot(halfway, viewDir), 0.0), f0);
    float3 diffuseWeight = (float3(1.0, 1.0, 1.0) - fresnel) * (1.0 - metallic);
    float3 directLight = EvaluateDirectLight(normal, viewDir, lightDir,
        lighting.directionalLightColor.rgb * lighting.directionalLightColor.a,
        albedo, metallic, roughness);
    float3 ambientFresnel = FresnelSchlickRoughness(max(dot(normal, viewDir), 0.0), f0, roughness);
    float3 ambientDiffuse = diffuseWeight * albedo * lighting.ambientColor.rgb * lighting.ambientColor.a;
    float3 ambientSpecular = ambientFresnel * lighting.environmentColor.rgb * lighting.environmentColor.a
        * max(lighting.pbrParams.y, 0.0);
    float3 color = ambientDiffuse + ambientSpecular + directLight;
    color = color / (color + float3(1.0, 1.0, 1.0));
    if(lighting.pbrParams.z > 0.5) color = pow(color, float3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2));
    return float4(color, baseColor.a);
}
