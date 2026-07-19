#version 450
#extension GL_GOOGLE_include_directive : require

#include "Graphics/RendererShaderLayout.inc"

// Bindless 변형: 머티리얼 텍스처를 set=0 의 고정 바인딩이 아니라 set=1 의 텍스처 배열에서
// per-draw 디스크립터 인덱스로 샘플한다. 인덱스는 push constant(textureIndices, emissiveColor.w)
// 로 전달되며 draw 당 상수(dynamically uniform)라 nonuniform 한정자가 필요 없다.

layout(location = 0) in vec2 fragUv;
layout(location = 1) in vec3 fragWorldPosition;
layout(location = 2) in vec3 fragNormal;
layout(location = 3) in vec4 fragTangent;
layout(location = 4) in vec4 fragLightSpacePosition;
layout(location = 0) out vec4 outColor;

// 그림자 맵은 bindless 배열이 아니라 기존 set=0 바인딩을 그대로 쓴다.
layout(set = 0, binding = DY_RENDERER_BINDING_SHADOW_MAP) uniform sampler2D shadowMap;

// set=1: bindless 머티리얼 텍스처 배열(VulkanDevice 가 디스크립터 인덱스에 텍스처를 기록).
layout(set = 1, binding = 0) uniform sampler2D bindlessTextures[DY_RENDERER_BINDLESS_TEXTURE_COUNT];

layout(set = 0, binding = DY_RENDERER_BINDING_LIGHTING_CONSTANTS) uniform RendererLighting {
    vec4 cameraPosition;
    vec4 directionalLightDirection;
    vec4 directionalLightColor;
    vec4 ambientColor;
    vec4 shadowParams;
    vec4 pbrParams;
    vec4 environmentColor;
    vec4 pointLightPositionRange;
    vec4 pointLightColorIntensity;
} lighting;

layout(push_constant) uniform DrawConstants {
    mat4 viewProjectionMatrix;
    mat4 modelMatrix;
    float drawMode;
    uint firstIndex;
    int vertexOffset;
    uint firstVertex;
    vec3 emissiveColor;
    float emissiveTextureIndex;
    vec4 baseColor;
    vec4 materialParams;
    vec4 textureIndices; // x=baseColor, y=metallicRoughness, z=normal, w=occlusion
} pushConstants;

const float PI = 3.14159265359;

vec4 SampleBindless(float index, vec2 uv) {
    return texture(bindlessTextures[int(index + 0.5)], uv);
}

float DistributionGGX(vec3 normal, vec3 halfway, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float ndoth = max(dot(normal, halfway), 0.0);
    float ndoth2 = ndoth * ndoth;
    float denom = ndoth2 * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 0.0001);
}

float GeometrySchlickGGX(float ndotv, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return ndotv / max(ndotv * (1.0 - k) + k, 0.0001);
}

float GeometrySmith(vec3 normal, vec3 viewDir, vec3 lightDir, float roughness) {
    float ndotv = max(dot(normal, viewDir), 0.0);
    float ndotl = max(dot(normal, lightDir), 0.0);
    return GeometrySchlickGGX(ndotv, roughness) * GeometrySchlickGGX(ndotl, roughness);
}

vec3 FresnelSchlick(float cosTheta, vec3 f0) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 FresnelSchlickRoughness(float cosTheta, vec3 f0, float roughness) {
    vec3 roughnessF0 = max(vec3(1.0 - roughness), f0);
    return f0 + (roughnessF0 - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 GetNormal() {
    vec3 normal = normalize(fragNormal);
    int textureFlags = int(pushConstants.drawMode + 0.5);
    if ((textureFlags & DY_RENDERER_TEXTURE_FLAG_NORMAL) == 0) {
        return normal;
    }

    float normalScale = pushConstants.materialParams.z;
    if (normalScale <= 0.0001) {
        return normal;
    }

    vec3 tangent = normalize(fragTangent.xyz - normal * dot(normal, fragTangent.xyz));
    vec3 bitangent = normalize(cross(normal, tangent)) * fragTangent.w;
    mat3 tbn = mat3(tangent, bitangent, normal);
    vec3 tangentNormal = SampleBindless(pushConstants.textureIndices.z, fragUv).xyz * 2.0 - 1.0;
    tangentNormal.xy *= normalScale;
    return normalize(tbn * tangentNormal);
}

float CalculateShadowVisibility(vec4 lightSpacePosition, vec3 normal, vec3 lightDir) {
    if (lighting.directionalLightDirection.w < 0.5) {
        return 1.0;
    }
    if ((int(pushConstants.drawMode + 0.5) & DY_RENDERER_TEXTURE_FLAG_RECEIVE_SHADOW) == 0) {
        return 1.0;
    }

    vec3 ndc = lightSpacePosition.xyz / lightSpacePosition.w;
    vec2 shadowUV = ndc.xy * vec2(0.5, -0.5) + 0.5;
    if (shadowUV.x < 0.0 || shadowUV.x > 1.0 ||
        shadowUV.y < 0.0 || shadowUV.y > 1.0 ||
        ndc.z < 0.0 || ndc.z > 1.0) {
        return 1.0;
    }

    float ndotl = max(dot(normal, lightDir), 0.0);
    float constantBias = max(lighting.shadowParams.x, 0.0);
    float slopeBias = max(lighting.shadowParams.y, 0.0) * (1.0 - ndotl);
    float normalBias = max(lighting.shadowParams.z, 0.0) * (1.0 - ndotl);
    float bias = max(slopeBias, constantBias) + normalBias;
    vec2 texelSize = 1.0 / vec2(textureSize(shadowMap, 0));
    float visibility = 0.0;
    int pcfRadius = clamp(int(lighting.shadowParams.w + 0.5), 0, 4);
    int sampleCount = 0;
    for (int y = -pcfRadius; y <= pcfRadius; ++y) {
        for (int x = -pcfRadius; x <= pcfRadius; ++x) {
            float sampledDepth = texture(shadowMap, shadowUV + vec2(x, y) * texelSize).r;
            visibility += (ndc.z - bias) > sampledDepth ? 0.0 : 1.0;
            sampleCount += 1;
        }
    }
    visibility /= max(float(sampleCount), 1.0);
    float strength = clamp(lighting.cameraPosition.w, 0.0, 1.0);
    return mix(1.0, visibility, strength);
}

void main() {
    int textureFlags = int(pushConstants.drawMode + 0.5);
    vec3 albedo = pushConstants.baseColor.rgb;
    if ((textureFlags & DY_RENDERER_TEXTURE_FLAG_BASE_COLOR) != 0) {
        albedo *= SampleBindless(pushConstants.textureIndices.x, fragUv).rgb;
    }

    float minRoughness = clamp(lighting.pbrParams.x, 0.01, 1.0);
    float ambientSpecularStrength = max(lighting.pbrParams.y, 0.0);
    float metallic = clamp(pushConstants.materialParams.x, 0.0, 1.0);
    float roughness = clamp(pushConstants.materialParams.y, minRoughness, 1.0);
    float occlusion = clamp(pushConstants.materialParams.w, 0.0, 1.0);
    if ((textureFlags & DY_RENDERER_TEXTURE_FLAG_METALLIC_ROUGHNESS) != 0) {
        vec4 metallicRoughness = SampleBindless(pushConstants.textureIndices.y, fragUv);
        roughness = clamp(roughness * metallicRoughness.g, minRoughness, 1.0);
        metallic = clamp(metallic * metallicRoughness.b, 0.0, 1.0);
    }
    if ((textureFlags & DY_RENDERER_TEXTURE_FLAG_OCCLUSION) != 0) {
        occlusion *= SampleBindless(pushConstants.textureIndices.w, fragUv).r;
    }

    vec3 normal = GetNormal();
    vec3 viewDir = normalize(lighting.cameraPosition.xyz - fragWorldPosition);
    bool usePointLight = lighting.pointLightColorIntensity.a > 0.0 && lighting.pointLightPositionRange.w > 0.0;
    vec3 lightDir = normalize(lighting.directionalLightDirection.xyz);
    vec3 radiance = lighting.directionalLightColor.rgb * lighting.directionalLightColor.a;
    if (usePointLight) {
        vec3 toLight = lighting.pointLightPositionRange.xyz - fragWorldPosition;
        float distanceToLight = length(toLight);
        lightDir = distanceToLight > 0.0001 ? toLight / distanceToLight : vec3(0.0, 0.0, 1.0);
        float range = max(lighting.pointLightPositionRange.w, 0.0001);
        float rangeFade = clamp(1.0 - distanceToLight / range, 0.0, 1.0);
        rangeFade *= rangeFade;
        float attenuation = rangeFade / max(1.0 + 0.18 * distanceToLight + 0.06 * distanceToLight * distanceToLight, 0.0001);
        radiance = lighting.pointLightColorIntensity.rgb * lighting.pointLightColorIntensity.a * attenuation;
    }
    vec3 halfway = normalize(viewDir + lightDir);

    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    float ndf = DistributionGGX(normal, halfway, roughness);
    float geometry = GeometrySmith(normal, viewDir, lightDir, roughness);
    vec3 fresnel = FresnelSchlick(max(dot(halfway, viewDir), 0.0), f0);
    vec3 specular = (ndf * geometry * fresnel) /
        max(4.0 * max(dot(normal, viewDir), 0.0) * max(dot(normal, lightDir), 0.0), 0.0001);

    vec3 kS = fresnel;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);
    float ndotl = max(dot(normal, lightDir), 0.0);
    float shadowVisibility = CalculateShadowVisibility(fragLightSpacePosition, normal, lightDir);
    vec3 directLight = (kD * albedo / PI + specular) * radiance * ndotl * shadowVisibility;
    vec3 ambientFresnel = FresnelSchlickRoughness(max(dot(normal, viewDir), 0.0), f0, roughness);
    vec3 ambientDiffuse = kD * albedo * lighting.ambientColor.rgb * lighting.ambientColor.a;
    vec3 ambientSpecular = ambientFresnel * lighting.environmentColor.rgb * lighting.environmentColor.a * ambientSpecularStrength;
    vec3 ambient = (ambientDiffuse + ambientSpecular) * occlusion;
    vec3 emissive = pushConstants.emissiveColor;
    if ((textureFlags & DY_RENDERER_TEXTURE_FLAG_EMISSIVE) != 0) {
        emissive *= SampleBindless(pushConstants.emissiveTextureIndex, fragUv).rgb;
    }
    vec3 color = ambient + directLight + emissive;

    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0 / 2.2));
    outColor = vec4(color, pushConstants.baseColor.a);
}
