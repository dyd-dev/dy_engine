// mesh_ps.hlsl - D3D12 Renderer lighting path matching the Vulkan GLSL shader.

cbuffer DrawConstants : register(b0)
{
    column_major float4x4 viewProjectionMatrix;
    column_major float4x4 modelMatrix;
    float drawMode;
    uint  firstIndex;
    int   vertexOffset;
    uint  firstVertex;
    float3 emissiveColor;
    float  baseColorTextureIndex;
    float4 baseColor;
    float4 materialParams;
};

cbuffer RendererLighting : register(b1)
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

Texture2D BaseColorTexture : register(t0, space0);
Texture2D MetallicRoughnessTexture : register(t1, space1);
Texture2D NormalTexture : register(t2, space1);
Texture2D OcclusionTexture : register(t3, space1);
Texture2D EmissiveTexture : register(t4, space1);
Texture2D ShadowMap        : register(t0, space4);
SamplerState LinearSampler : register(s0);

struct PSInput
{
    float4 position           : SV_POSITION;
    float2 uv                 : TEXCOORD0;
    float3 worldPosition      : TEXCOORD1;
    float3 worldNormal        : TEXCOORD2;
    float4 worldTangent       : TEXCOORD3;
    float4 lightSpacePosition : TEXCOORD4;
};

#define DY_TEXTURE_FLAG_BASE_COLOR 1u
#define DY_TEXTURE_FLAG_METALLIC_ROUGHNESS 2u
#define DY_TEXTURE_FLAG_NORMAL 4u
#define DY_TEXTURE_FLAG_OCCLUSION 8u
#define DY_TEXTURE_FLAG_EMISSIVE 16u
#define DY_TEXTURE_FLAG_RECEIVE_SHADOW 32u

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
    if ((textureFlags & DY_TEXTURE_FLAG_NORMAL) == 0u)
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

// Manual-compare PCF, matching mesh_ps.glsl. RHI clip-space is y-up while
// texture coordinates are v-down, so shadow sampling flips the y coordinate.
float CalculateShadowVisibility(float4 lightSpacePosition, float3 normal, float3 lightDir)
{
    if (directionalLightDirection.w < 0.5)
        return 1.0;
    uint flags = (uint)(drawMode + 0.5);
    if ((flags & DY_TEXTURE_FLAG_RECEIVE_SHADOW) == 0u)
        return 1.0;

    float3 ndc = lightSpacePosition.xyz / lightSpacePosition.w;
    float2 shadowUV = ndc.xy * float2(0.5, -0.5) + 0.5;
    if (shadowUV.x < 0.0 || shadowUV.x > 1.0 ||
        shadowUV.y < 0.0 || shadowUV.y > 1.0 ||
        ndc.z < 0.0 || ndc.z > 1.0)
        return 1.0;

    float ndotl = max(dot(normal, lightDir), 0.0);
    float constantBias = max(shadowParams.x, 0.0);
    float slopeBias = max(shadowParams.y, 0.0) * (1.0 - ndotl);
    float normalBias = max(shadowParams.z, 0.0) * (1.0 - ndotl);
    float bias = max(slopeBias, constantBias) + normalBias;

    uint width, height;
    ShadowMap.GetDimensions(width, height);
    float2 texelSize = 1.0 / float2(width, height);
    int pcfRadius = clamp((int)(shadowParams.w + 0.5), 0, 4);
    float visibility = 0.0;
    int sampleCount = 0;
    for (int y = -pcfRadius; y <= pcfRadius; ++y)
    {
        for (int x = -pcfRadius; x <= pcfRadius; ++x)
        {
            float sampledDepth = ShadowMap.SampleLevel(LinearSampler, shadowUV + float2(x, y) * texelSize, 0).r;
            visibility += (ndc.z - bias) > sampledDepth ? 0.0 : 1.0;
            sampleCount += 1;
        }
    }
    visibility /= max((float)sampleCount, 1.0);
    float strength = saturate(cameraPosition.w);
    return lerp(1.0, visibility, strength);
}

float4 main(PSInput input) : SV_TARGET
{
    uint textureFlags = (uint)(drawMode + 0.5);
    float3 albedo = baseColor.rgb;
    if ((textureFlags & DY_TEXTURE_FLAG_BASE_COLOR) != 0u)
    {
        albedo *= SampleBaseColorTexture(input.uv).rgb;
    }

    float minRoughness = clamp(pbrParams.x, 0.01, 1.0);
    float ambientSpecularStrength = max(pbrParams.y, 0.0);
    float metallic = clamp(materialParams.x, 0.0, 1.0);
    float roughness = clamp(materialParams.y, minRoughness, 1.0);
    float occlusion = clamp(materialParams.w, 0.0, 1.0);
    if ((textureFlags & DY_TEXTURE_FLAG_METALLIC_ROUGHNESS) != 0u)
    {
        float4 metallicRoughness = MetallicRoughnessTexture.Sample(LinearSampler, input.uv);
        roughness = clamp(roughness * metallicRoughness.g, minRoughness, 1.0);
        metallic = clamp(metallic * metallicRoughness.b, 0.0, 1.0);
    }
    if ((textureFlags & DY_TEXTURE_FLAG_OCCLUSION) != 0u)
    {
        occlusion *= OcclusionTexture.Sample(LinearSampler, input.uv).r;
    }
    float3 emissive = emissiveColor;
    if ((textureFlags & DY_TEXTURE_FLAG_EMISSIVE) != 0u)
    {
        emissive *= EmissiveTexture.Sample(LinearSampler, input.uv).rgb;
    }

    float3 normal = GetNormal(input, textureFlags);
    float3 viewDir = normalize(cameraPosition.xyz - input.worldPosition);
    bool usePointLight = pointLightColorIntensity.a > 0.0 && pointLightPositionRange.w > 0.0;
    float3 lightDir = normalize(directionalLightDirection.xyz);
    float3 radiance = directionalLightColor.rgb * directionalLightColor.a;
    if (usePointLight)
    {
        float3 toLight = pointLightPositionRange.xyz - input.worldPosition;
        float distanceToLight = length(toLight);
        lightDir = distanceToLight > 0.0001 ? toLight / distanceToLight : float3(0.0, 0.0, 1.0);
        float range = max(pointLightPositionRange.w, 0.0001);
        float rangeFade = saturate(1.0 - distanceToLight / range);
        rangeFade *= rangeFade;
        float attenuation = rangeFade / max(1.0 + 0.18 * distanceToLight + 0.06 * distanceToLight * distanceToLight, 0.0001);
        radiance = pointLightColorIntensity.rgb * pointLightColorIntensity.a * attenuation;
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
    float shadowVisibility = CalculateShadowVisibility(input.lightSpacePosition, normal, lightDir);
    float3 directLight = (kD * albedo / PI + specular) * radiance * ndotl * shadowVisibility;
    float3 ambientFresnel = FresnelSchlickRoughness(max(dot(normal, viewDir), 0.0), f0, roughness);
    float3 ambientDiffuse = kD * albedo * ambientColor.rgb * ambientColor.a;
    float3 ambientSpecular = ambientFresnel * environmentColor.rgb * environmentColor.a * ambientSpecularStrength;
    float3 ambient = (ambientDiffuse + ambientSpecular) * occlusion;
    float3 color = ambient + directLight + emissive;

    color = color / (color + float3(1.0, 1.0, 1.0));
    color = pow(color, float3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2));
    return float4(color, baseColor.a);
}
