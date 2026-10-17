// mesh_ps.hlsl - D3D12 Renderer lighting path matching the Vulkan GLSL shader.

cbuffer DrawConstants : register(
    b10,
    space0)
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

struct LightingConstants
{
    float4 cameraPosition;
    float4 directionalLightDirection;
    float4 directionalLightColor;
    float4 ambientColor;
    float4 pbrParams;
    float4 environmentColor;
};

cbuffer RendererLighting : register(b1, space0) { LightingConstants lighting; };

Texture2D BaseColorTexture : register(
    t0,
    space0);
Texture2D MetallicRoughnessTexture : register(
    t4,
    space0);
Texture2D NormalTexture : register(
    t5,
    space0);
Texture2D OcclusionTexture : register(
    t6,
    space0);
Texture2D EmissiveTexture : register(
    t7,
    space0);
SamplerState LinearSampler : register(
    s8,
    space0);

struct PSInput
{
    float4 position           : SV_POSITION;
    float2 uv                 : TEXCOORD0;
    float3 worldPosition      : TEXCOORD1;
    float3 worldNormal        : TEXCOORD2;
    float4 worldTangent       : TEXCOORD3;
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
    if ((textureFlags & 4) == 0u)
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
    float3 f0 = lerp(float3(0.04,0.04,0.04), albedo, metallic);
    float ndf = DistributionGGX(normal, halfway, roughness);
    float geometry = GeometrySmith(normal, viewDir, lightDir, roughness);
    float3 fresnel = FresnelSchlick(max(dot(halfway, viewDir), 0.0), f0);
    float3 specular = (ndf * geometry * fresnel) / max(4.0 * ndotv * ndotl, 0.0001);
    float3 diffuseWeight = (float3(1.0,1.0,1.0) - fresnel) * (1.0 - metallic);
    return (diffuseWeight * albedo / PI + specular) * incidentIlluminance * ndotl;
}

float4 main(PSInput input) : SV_TARGET
{
    uint textureFlags = drawTextureFlags;
    float3 albedo = baseColor.rgb;
    if ((textureFlags & 1) != 0u)
    {
        albedo *= SampleBaseColorTexture(input.uv).rgb;
    }

    float minRoughness = clamp(lighting.pbrParams.x, 0.01, 1.0);
    float ambientSpecularStrength = max(lighting.pbrParams.y, 0.0);
    float metallic = clamp(materialParams.x, 0.0, 1.0);
    float roughness = clamp(materialParams.y, minRoughness, 1.0);
    float occlusion = clamp(materialParams.w, 0.0, 1.0);
    if ((textureFlags & 2) != 0u)
    {
        float4 metallicRoughness = MetallicRoughnessTexture.Sample(LinearSampler, input.uv);
        roughness = clamp(roughness * metallicRoughness.g, minRoughness, 1.0);
        metallic = clamp(metallic * metallicRoughness.b, 0.0, 1.0);
    }
    if ((textureFlags & 8) != 0u)
    {
        occlusion *= OcclusionTexture.Sample(LinearSampler, input.uv).r;
    }
    float3 emissive = emissiveColor.rgb;
    if ((textureFlags & 16) != 0u)
    {
        emissive *= EmissiveTexture.Sample(LinearSampler, input.uv).rgb;
    }

    float3 normal = GetNormal(input, textureFlags);
    float3 viewDir = normalize(lighting.cameraPosition.xyz - input.worldPosition);
    float3 lightDir = normalize(lighting.directionalLightDirection.xyz);

    float3 halfway = normalize(viewDir + lightDir);
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 fresnel = FresnelSchlick(max(dot(halfway, viewDir), 0.0), f0);

    float3 kS = fresnel;
    float3 kD = (float3(1.0, 1.0, 1.0) - kS) * (1.0 - metallic);
    float3 directLight = EvaluateDirectLight(normal, viewDir, lightDir,
        lighting.directionalLightColor.rgb * max(lighting.directionalLightColor.a, 0.0),
        albedo, metallic, roughness);
    float3 ambientFresnel = FresnelSchlickRoughness(max(dot(normal, viewDir), 0.0), f0, roughness);
    float3 ambientDiffuse = kD * albedo * lighting.ambientColor.rgb * lighting.ambientColor.a;
    float3 ambientSpecular = ambientFresnel * lighting.environmentColor.rgb * lighting.environmentColor.a * ambientSpecularStrength;
    float3 ambient = (ambientDiffuse + ambientSpecular) * occlusion;
    float3 color = ambient + directLight + emissive;

    color = color / (color + float3(1.0, 1.0, 1.0));
    if (lighting.pbrParams.z > 0.5) color = pow(color, float3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2));
    return float4(color, baseColor.a);
}
