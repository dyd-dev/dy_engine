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

struct RendererDirectionalLight {
    vec4 directionIlluminance;
    vec4 color;
};

struct RendererPointLight {
    vec4 positionRange;
    vec4 colorIntensity;
};

struct RendererSpotLight {
    vec4 positionRange;
    vec4 directionOuterCos;
    vec4 colorIntensity;
    vec4 coneParams;
};

struct RendererRectAreaLight {
    vec4 positionLuminance;
    vec4 directionWidth;
    vec4 upHeight;
    vec4 color;
};

struct RendererDiscAreaLight {
    vec4 positionLuminance;
    vec4 directionRadius;
    vec4 up;
    vec4 color;
};

layout(std140, set = 0, binding = DY_RENDERER_BINDING_LIGHTING_CONSTANTS) uniform RendererLighting {
    vec4 cameraPosition;
    vec4 ambientColor;
    vec4 shadowParams;
    vec4 pbrParams;
    vec4 environmentColor;
    vec4 lightCounts;
    vec4 shadowLight;
    vec4 areaLightCounts;
    RendererDirectionalLight directionalLights[DY_RENDERER_MAX_DIRECTIONAL_LIGHTS];
    RendererPointLight pointLights[DY_RENDERER_MAX_POINT_LIGHTS];
    RendererSpotLight spotLights[DY_RENDERER_MAX_SPOT_LIGHTS];
    RendererRectAreaLight rectAreaLights[DY_RENDERER_MAX_RECT_AREA_LIGHTS];
    RendererDiscAreaLight discAreaLights[DY_RENDERER_MAX_DISC_AREA_LIGHTS];
} lighting;

layout(std140, set = 0, binding = DY_RENDERER_BINDING_SHADOW_MATRIX) uniform ShadowMatrix {
    mat4 lightViewProjectionMatrices[6];
    vec4 cascadeSplits;
    vec4 shadowInfo;
    vec4 pcssParams;
    mat4 cameraViewMatrix;
} shadowMatrix;

layout(std140, set = 0, binding = DY_VULKAN_BINDING_DRAW_CONSTANTS) uniform VulkanDrawConstants {
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

int ResolvePointShadowFace(vec3 fromLight) {
    vec3 absoluteDirection = abs(fromLight);
    if (absoluteDirection.x >= absoluteDirection.y && absoluteDirection.x >= absoluteDirection.z) {
        return fromLight.x >= 0.0 ? 0 : 1;
    }
    if (absoluteDirection.y >= absoluteDirection.x && absoluteDirection.y >= absoluteDirection.z) {
        return fromLight.y >= 0.0 ? 2 : 3;
    }
    return fromLight.z >= 0.0 ? 4 : 5;
}

int ResolveShadowViewIndex(vec3 worldPosition, vec3 lightDir) {
    int shadowType = int(shadowMatrix.shadowInfo.x + 0.5);
    int viewCount = clamp(int(shadowMatrix.shadowInfo.y + 0.5), 0, 6);
    if (shadowType == 1) {
        float viewDepth = max(-(shadowMatrix.cameraViewMatrix * vec4(worldPosition, 1.0)).z, 0.0);
        int cascadeIndex = 0;
        for (int cascade = 0; cascade < min(viewCount, 4); ++cascade) {
            cascadeIndex = cascade;
            if (viewDepth <= shadowMatrix.cascadeSplits[cascade]) {
                break;
            }
        }
        return cascadeIndex;
    }
    if (shadowType == 2) {
        return ResolvePointShadowFace(-lightDir);
    }
    return 0;
}

vec2 ShadowAtlasUv(vec2 localUv, int viewIndex, out vec2 tileMin, out vec2 tileMax) {
    int columns = max(int(shadowMatrix.shadowInfo.z + 0.5), 1);
    int rows = max(int(shadowMatrix.shadowInfo.w + 0.5), 1);
    vec2 tileScale = vec2(1.0 / float(columns), 1.0 / float(rows));
    ivec2 tile = ivec2(viewIndex % columns, viewIndex / columns);
    tileMin = vec2(tile) * tileScale;
    tileMax = tileMin + tileScale;
    return tileMin + localUv * tileScale;
}

float FindAverageBlockerDepth(
    vec2 atlasUv,
    vec2 tileMin,
    vec2 tileMax,
    float receiverDepth,
    float bias,
    int searchRadius) {
    vec2 texelSize = 1.0 / vec2(textureSize(shadowMap, 0));
    vec2 safeMin = tileMin + texelSize * 0.5;
    vec2 safeMax = tileMax - texelSize * 0.5;
    float blockerDepthSum = 0.0;
    int blockerCount = 0;
    for (int y = -4; y <= 4; ++y) {
        for (int x = -4; x <= 4; ++x) {
            if (abs(x) > searchRadius || abs(y) > searchRadius) {
                continue;
            }
            vec2 sampleUv = clamp(atlasUv + vec2(x, y) * texelSize, safeMin, safeMax);
            float sampledDepth = texture(shadowMap, sampleUv).r;
            if (sampledDepth < receiverDepth - bias) {
                blockerDepthSum += sampledDepth;
                blockerCount += 1;
            }
        }
    }
    return blockerCount > 0 ? blockerDepthSum / float(blockerCount) : -1.0;
}

float CalculateShadowVisibility(vec3 worldPosition, vec3 normal, vec3 lightDir) {
    if (lighting.shadowLight.w < 0.5 ||
        (int(pushConstants.drawMode + 0.5) & DY_RENDERER_TEXTURE_FLAG_RECEIVE_SHADOW) == 0) {
        return 1.0;
    }

    int viewIndex = ResolveShadowViewIndex(worldPosition, lightDir);
    vec4 lightSpacePosition = shadowMatrix.lightViewProjectionMatrices[viewIndex] * vec4(worldPosition, 1.0);
    if (abs(lightSpacePosition.w) <= 0.000001) {
        return 1.0;
    }
    vec3 ndc = lightSpacePosition.xyz / lightSpacePosition.w;
    vec2 localUv = ndc.xy * 0.5 + 0.5;
    if (localUv.x < 0.0 || localUv.x > 1.0 ||
        localUv.y < 0.0 || localUv.y > 1.0 ||
        ndc.z < 0.0 || ndc.z > 1.0) {
        return 1.0;
    }

    float ndotl = max(dot(normal, lightDir), 0.0);
    float constantBias = max(lighting.shadowParams.x, 0.0);
    float slopeBias = max(lighting.shadowParams.y, 0.0) * (1.0 - ndotl);
    float normalBias = max(lighting.shadowParams.z, 0.0) * (1.0 - ndotl);
    float bias = max(slopeBias, constantBias) + normalBias;

    vec2 tileMin;
    vec2 tileMax;
    vec2 atlasUv = ShadowAtlasUv(localUv, viewIndex, tileMin, tileMax);
    int baseRadius = clamp(int(lighting.shadowParams.w + 0.5), 0, 4);
    int blockerSearchRadius = clamp(
        int(shadowMatrix.pcssParams.y + 0.5) + baseRadius, 1, 4);
    float averageBlockerDepth = FindAverageBlockerDepth(
        atlasUv, tileMin, tileMax, ndc.z, bias, blockerSearchRadius);

    float filterRadius = float(baseRadius);
    if (averageBlockerDepth > 0.0) {
        int columns = max(int(shadowMatrix.shadowInfo.z + 0.5), 1);
        int rows = max(int(shadowMatrix.shadowInfo.w + 0.5), 1);
        vec2 atlasSize = vec2(textureSize(shadowMap, 0));
        float tileResolution = min(atlasSize.x / float(columns), atlasSize.y / float(rows));
        float penumbra = max((ndc.z - averageBlockerDepth) / max(averageBlockerDepth, 0.0001), 0.0);
        filterRadius = clamp(
            filterRadius + penumbra * max(shadowMatrix.pcssParams.x, 0.0) * tileResolution,
            filterRadius,
            max(shadowMatrix.pcssParams.z, 1.0));
    }
    int integerFilterRadius = clamp(int(ceil(filterRadius)), 0, 8);

    vec2 texelSize = 1.0 / vec2(textureSize(shadowMap, 0));
    vec2 safeMin = tileMin + texelSize * 0.5;
    vec2 safeMax = tileMax - texelSize * 0.5;
    float visibility = 0.0;
    int sampleCount = 0;
    for (int y = -8; y <= 8; ++y) {
        for (int x = -8; x <= 8; ++x) {
            if (abs(x) > integerFilterRadius || abs(y) > integerFilterRadius) {
                continue;
            }
            vec2 sampleUv = clamp(atlasUv + vec2(x, y) * texelSize, safeMin, safeMax);
            float sampledDepth = texture(shadowMap, sampleUv).r;
            visibility += (ndc.z - bias) > sampledDepth ? 0.0 : 1.0;
            sampleCount += 1;
        }
    }
    visibility /= max(float(sampleCount), 1.0);
    float strength = clamp(lighting.shadowLight.z, 0.0, 1.0);
    return mix(1.0, visibility, strength);
}

float PunctualAttenuation(float distanceToLight, float range) {
    if (range <= 0.0 || distanceToLight >= range) {
        return 0.0;
    }
    float distanceSquared = max(distanceToLight * distanceToLight, 0.0001);
    float ratio = distanceToLight / range;
    float ratioSquared = ratio * ratio;
    float window = max(1.0 - ratioSquared * ratioSquared, 0.0);
    return (window * window) / distanceSquared;
}

float SpotConeAttenuation(float cosTheta, float innerCos, float outerCos) {
    float denominator = max(innerCos - outerCos, 0.0001);
    float t = clamp((cosTheta - outerCos) / denominator, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

vec3 EvaluateDirectLight(
    vec3 normal,
    vec3 viewDir,
    vec3 lightDir,
    vec3 incidentIlluminance,
    vec3 albedo,
    float metallic,
    float roughness,
    float visibility) {
    float ndotl = max(dot(normal, lightDir), 0.0);
    float ndotv = max(dot(normal, viewDir), 0.0);
    if (ndotl <= 0.0 || ndotv <= 0.0 || visibility <= 0.0) {
        return vec3(0.0);
    }

    vec3 halfwayVector = viewDir + lightDir;
    float halfwayLengthSquared = dot(halfwayVector, halfwayVector);
    if (halfwayLengthSquared <= 0.000001) {
        return vec3(0.0);
    }
    vec3 halfway = halfwayVector * inversesqrt(halfwayLengthSquared);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    float ndf = DistributionGGX(normal, halfway, roughness);
    float geometry = GeometrySmith(normal, viewDir, lightDir, roughness);
    vec3 fresnel = FresnelSchlick(max(dot(halfway, viewDir), 0.0), f0);
    vec3 specular = (ndf * geometry * fresnel) / max(4.0 * ndotv * ndotl, 0.0001);
    vec3 diffuseWeight = (vec3(1.0) - fresnel) * (1.0 - metallic);
    return (diffuseWeight * albedo / PI + specular) * incidentIlluminance * ndotl * visibility;
}

vec3 EvaluateAreaSample(
    vec3 normal,
    vec3 viewDir,
    vec3 samplePosition,
    vec3 emitterDirection,
    vec3 emittedRadiance,
    float sampleArea,
    vec3 albedo,
    float metallic,
    float roughness) {
    vec3 toLight = samplePosition - fragWorldPosition;
    float distanceSquared = dot(toLight, toLight);
    if (distanceSquared <= 0.000001 || sampleArea <= 0.0) {
        return vec3(0.0);
    }
    vec3 lightDir = toLight * inversesqrt(distanceSquared);
    float emitterCosine = max(dot(emitterDirection, -lightDir), 0.0);
    vec3 incidentIlluminance = emittedRadiance * emitterCosine * sampleArea / distanceSquared;
    return EvaluateDirectLight(
        normal, viewDir, lightDir, incidentIlluminance, albedo, metallic, roughness, 1.0);
}

vec3 EvaluateRectAreaLight(
    RendererRectAreaLight light,
    vec3 normal,
    vec3 viewDir,
    vec3 albedo,
    float metallic,
    float roughness) {
    vec3 emitterDirection = normalize(light.directionWidth.xyz);
    vec3 emitterUp = normalize(light.upHeight.xyz);
    vec3 emitterRight = normalize(cross(emitterDirection, emitterUp));
    emitterUp = normalize(cross(emitterRight, emitterDirection));
    float width = max(light.directionWidth.w, 0.0);
    float height = max(light.upHeight.w, 0.0);
    float sampleArea = width * height * 0.25;
    vec3 emittedRadiance = light.color.rgb * max(light.positionLuminance.w, 0.0);
    vec3 result = vec3(0.0);
    for (int sampleIndex = 0; sampleIndex < 4; ++sampleIndex) {
        float x = (sampleIndex & 1) == 0 ? -0.25 : 0.25;
        float y = (sampleIndex & 2) == 0 ? -0.25 : 0.25;
        vec3 samplePosition = light.positionLuminance.xyz +
            emitterRight * (x * width) + emitterUp * (y * height);
        result += EvaluateAreaSample(
            normal, viewDir, samplePosition, emitterDirection, emittedRadiance,
            sampleArea, albedo, metallic, roughness);
    }
    return result;
}

vec3 EvaluateDiscAreaLight(
    RendererDiscAreaLight light,
    vec3 normal,
    vec3 viewDir,
    vec3 albedo,
    float metallic,
    float roughness) {
    const int sampleCount = 8;
    const float goldenAngle = 2.39996323;
    vec3 emitterDirection = normalize(light.directionRadius.xyz);
    vec3 emitterUp = normalize(light.up.xyz);
    vec3 emitterRight = normalize(cross(emitterDirection, emitterUp));
    emitterUp = normalize(cross(emitterRight, emitterDirection));
    float radius = max(light.directionRadius.w, 0.0);
    float sampleArea = PI * radius * radius / float(sampleCount);
    vec3 emittedRadiance = light.color.rgb * max(light.positionLuminance.w, 0.0);
    vec3 result = vec3(0.0);
    for (int sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
        float normalizedRadius = sqrt((float(sampleIndex) + 0.5) / float(sampleCount));
        float angle = float(sampleIndex) * goldenAngle;
        vec2 discOffset = vec2(cos(angle), sin(angle)) * (normalizedRadius * radius);
        vec3 samplePosition = light.positionLuminance.xyz +
            emitterRight * discOffset.x + emitterUp * discOffset.y;
        result += EvaluateAreaSample(
            normal, viewDir, samplePosition, emitterDirection, emittedRadiance,
            sampleArea, albedo, metallic, roughness);
    }
    return result;
}

vec3 EnvironmentBrdfApproximation(vec3 f0, float roughness, float ndotv) {
    vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * ndotv)) * r.x + r.y;
    vec2 scaleBias = vec2(-1.04, 1.04) * a004 + r.zw;
    return f0 * scaleBias.x + scaleBias.y;
}

vec3 AcesFitted(vec3 color) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
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
    vec3 viewVector = lighting.cameraPosition.xyz - fragWorldPosition;
    float viewLengthSquared = dot(viewVector, viewVector);
    vec3 viewDir = viewLengthSquared > 0.000001
        ? viewVector * inversesqrt(viewLengthSquared)
        : normal;
    int shadowType = int(lighting.shadowLight.x + 0.5);
    int shadowIndex = int(lighting.shadowLight.y + 0.5);
    vec3 directLight = vec3(0.0);

    int directionalCount = clamp(int(lighting.lightCounts.x + 0.5), 0, DY_RENDERER_MAX_DIRECTIONAL_LIGHTS);
    for (int lightIndex = 0; lightIndex < directionalCount; ++lightIndex) {
        RendererDirectionalLight light = lighting.directionalLights[lightIndex];
        vec3 lightDir = normalize(light.directionIlluminance.xyz);
        vec3 incidentIlluminance = light.color.rgb * max(light.directionIlluminance.w, 0.0);
        float visibility = shadowType == 1 && shadowIndex == lightIndex
            ? CalculateShadowVisibility(fragWorldPosition, normal, lightDir)
            : 1.0;
        directLight += EvaluateDirectLight(
            normal, viewDir, lightDir, incidentIlluminance, albedo, metallic, roughness, visibility);
    }

    int pointCount = clamp(int(lighting.lightCounts.y + 0.5), 0, DY_RENDERER_MAX_POINT_LIGHTS);
    for (int lightIndex = 0; lightIndex < pointCount; ++lightIndex) {
        RendererPointLight light = lighting.pointLights[lightIndex];
        vec3 toLight = light.positionRange.xyz - fragWorldPosition;
        float distanceToLight = length(toLight);
        if (distanceToLight <= 0.0001) {
            continue;
        }
        vec3 lightDir = toLight / distanceToLight;
        float attenuation = PunctualAttenuation(distanceToLight, light.positionRange.w);
        vec3 incidentIlluminance = light.colorIntensity.rgb * max(light.colorIntensity.w, 0.0) * attenuation;
        float visibility = shadowType == 2 && shadowIndex == lightIndex
            ? CalculateShadowVisibility(fragWorldPosition, normal, lightDir)
            : 1.0;
        directLight += EvaluateDirectLight(
            normal, viewDir, lightDir, incidentIlluminance, albedo, metallic, roughness, visibility);
    }

    int spotCount = clamp(int(lighting.lightCounts.z + 0.5), 0, DY_RENDERER_MAX_SPOT_LIGHTS);
    for (int lightIndex = 0; lightIndex < spotCount; ++lightIndex) {
        RendererSpotLight light = lighting.spotLights[lightIndex];
        vec3 toLight = light.positionRange.xyz - fragWorldPosition;
        float distanceToLight = length(toLight);
        if (distanceToLight <= 0.0001) {
            continue;
        }
        vec3 lightDir = toLight / distanceToLight;
        float distanceAttenuation = PunctualAttenuation(distanceToLight, light.positionRange.w);
        float cosTheta = dot(-lightDir, normalize(light.directionOuterCos.xyz));
        float coneAttenuation = SpotConeAttenuation(
            cosTheta, light.coneParams.x, light.directionOuterCos.w);
        vec3 incidentIlluminance =
            light.colorIntensity.rgb * max(light.colorIntensity.w, 0.0) *
            distanceAttenuation * coneAttenuation;
        float visibility = shadowType == 3 && shadowIndex == lightIndex
            ? CalculateShadowVisibility(fragWorldPosition, normal, lightDir)
            : 1.0;
        directLight += EvaluateDirectLight(
            normal, viewDir, lightDir, incidentIlluminance, albedo, metallic, roughness, visibility);
    }

    int rectAreaCount = clamp(int(lighting.areaLightCounts.x + 0.5), 0, DY_RENDERER_MAX_RECT_AREA_LIGHTS);
    for (int lightIndex = 0; lightIndex < rectAreaCount; ++lightIndex) {
        directLight += EvaluateRectAreaLight(
            lighting.rectAreaLights[lightIndex], normal, viewDir, albedo, metallic, roughness);
    }

    int discAreaCount = clamp(int(lighting.areaLightCounts.y + 0.5), 0, DY_RENDERER_MAX_DISC_AREA_LIGHTS);
    for (int lightIndex = 0; lightIndex < discAreaCount; ++lightIndex) {
        directLight += EvaluateDiscAreaLight(
            lighting.discAreaLights[lightIndex], normal, viewDir, albedo, metallic, roughness);
    }

    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 ambientFresnel = FresnelSchlickRoughness(max(dot(normal, viewDir), 0.0), f0, roughness);
    vec3 ambientDiffuseWeight = (vec3(1.0) - ambientFresnel) * (1.0 - metallic);
    vec3 ambientDiffuse = ambientDiffuseWeight * albedo * lighting.ambientColor.rgb * lighting.ambientColor.a;
    vec3 environmentBrdf = EnvironmentBrdfApproximation(f0, roughness, max(dot(normal, viewDir), 0.0));
    vec3 ambientSpecular = environmentBrdf * lighting.environmentColor.rgb * lighting.environmentColor.a * ambientSpecularStrength;
    vec3 ambient = (ambientDiffuse + ambientSpecular) * occlusion;
    vec3 emissive = pushConstants.emissiveColor;
    if ((textureFlags & DY_RENDERER_TEXTURE_FLAG_EMISSIVE) != 0) {
        emissive *= SampleBindless(pushConstants.emissiveTextureIndex, fragUv).rgb;
    }
    vec3 color = ambient + directLight + emissive;

    if (lighting.pbrParams.w > 0.5) {
        color = AcesFitted(max(color, vec3(0.0)));
    }
    if (lighting.pbrParams.z > 0.5) {
        color = pow(max(color, vec3(0.0)), vec3(1.0 / 2.2));
    }
    outColor = vec4(color, pushConstants.baseColor.a);
}
