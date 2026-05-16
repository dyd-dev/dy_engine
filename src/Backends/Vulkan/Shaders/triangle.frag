#version 450

layout(location = 0) in vec2 fragUv;
layout(location = 1) in vec3 fragWorldPosition;
layout(location = 2) in vec3 fragNormal;
layout(location = 3) in vec4 fragTangent;
layout(location = 4) in vec4 fragLightSpacePosition;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D baseColorTexture;
layout(set = 0, binding = 2) uniform sampler2D shadowMap;
layout(set = 0, binding = 6) uniform sampler2D metallicRoughnessTexture;
layout(set = 0, binding = 7) uniform sampler2D normalTexture;
layout(set = 0, binding = 8) uniform sampler2D occlusionTexture;
layout(set = 0, binding = 9) uniform sampler2D emissiveTexture;

layout(set = 0, binding = 1) uniform RendererLighting {
    vec4 cameraPosition;
    vec4 directionalLightDirection;
    vec4 directionalLightColor;
    vec4 ambientColor;
} lighting;

layout(push_constant) uniform DrawConstants {
    mat4 viewProjectionMatrix;
    mat4 modelMatrix;
    float drawMode;
    uint firstIndex;
    int vertexOffset;
    uint firstVertex;
    vec3 emissiveColor;
    vec4 baseColor;
    vec4 materialParams;
} pushConstants;

const float PI = 3.14159265359;
const int ReceiveShadowFlag = 32;

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

vec3 GetNormal() {
    vec3 normal = normalize(fragNormal);
    int textureFlags = int(pushConstants.drawMode + 0.5);
    if ((textureFlags & 4) == 0) {
        return normal;
    }

    float normalScale = pushConstants.materialParams.z;
    if (normalScale <= 0.0001) {
        return normal;
    }

    vec3 tangent = normalize(fragTangent.xyz - normal * dot(normal, fragTangent.xyz));
    vec3 bitangent = normalize(cross(normal, tangent)) * fragTangent.w;
    mat3 tbn = mat3(tangent, bitangent, normal);
    vec3 tangentNormal = texture(normalTexture, fragUv).xyz * 2.0 - 1.0;
    tangentNormal.xy *= normalScale;
    return normalize(tbn * tangentNormal);
}

float CalculateShadowVisibility(vec4 lightSpacePosition, vec3 normal, vec3 lightDir) {
    if (lighting.directionalLightDirection.w < 0.5) {
        return 1.0;
    }
    if ((int(pushConstants.drawMode + 0.5) & ReceiveShadowFlag) == 0) {
        return 1.0;
    }

    vec3 ndc = lightSpacePosition.xyz / lightSpacePosition.w;
    vec2 shadowUV = ndc.xy * 0.5 + 0.5;
    if (shadowUV.x < 0.0 || shadowUV.x > 1.0 ||
        shadowUV.y < 0.0 || shadowUV.y > 1.0 ||
        ndc.z < 0.0 || ndc.z > 1.0) {
        return 1.0;
    }

    float ndotl = max(dot(normal, lightDir), 0.0);
    float bias = max(0.003 * (1.0 - ndotl), 0.0007);
    vec2 texelSize = 1.0 / vec2(textureSize(shadowMap, 0));
    float visibility = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float sampledDepth = texture(shadowMap, shadowUV + vec2(x, y) * texelSize).r;
            visibility += (ndc.z - bias) > sampledDepth ? 0.0 : 1.0;
        }
    }
    visibility /= 9.0;
    float strength = clamp(lighting.cameraPosition.w, 0.0, 1.0);
    return mix(1.0, visibility, strength);
}

void main() {
    int textureFlags = int(pushConstants.drawMode + 0.5);
    vec3 albedo = pushConstants.baseColor.rgb;
    if ((textureFlags & 1) != 0) {
        albedo *= texture(baseColorTexture, fragUv).rgb;
    }

    float metallic = clamp(pushConstants.materialParams.x, 0.0, 1.0);
    float roughness = clamp(pushConstants.materialParams.y, 0.04, 1.0);
    float occlusion = clamp(pushConstants.materialParams.w, 0.0, 1.0);
    if ((textureFlags & 2) != 0) {
        vec4 metallicRoughness = texture(metallicRoughnessTexture, fragUv);
        roughness = clamp(roughness * metallicRoughness.g, 0.04, 1.0);
        metallic = clamp(metallic * metallicRoughness.b, 0.0, 1.0);
    }
    if ((textureFlags & 8) != 0) {
        occlusion *= texture(occlusionTexture, fragUv).r;
    }

    vec3 normal = GetNormal();
    vec3 viewDir = normalize(lighting.cameraPosition.xyz - fragWorldPosition);
    vec3 lightDir = normalize(lighting.directionalLightDirection.xyz);
    vec3 halfway = normalize(viewDir + lightDir);
    vec3 radiance = lighting.directionalLightColor.rgb * lighting.directionalLightColor.a;

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
    vec3 ambient = albedo * lighting.ambientColor.rgb * lighting.ambientColor.a * occlusion;
    vec3 emissive = pushConstants.emissiveColor;
    if ((textureFlags & 16) != 0) {
        emissive *= texture(emissiveTexture, fragUv).rgb;
    }
    vec3 color = ambient + directLight + emissive;

    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0 / 2.2));
    outColor = vec4(color, pushConstants.baseColor.a);
}
