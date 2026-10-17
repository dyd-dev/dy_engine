#version 450

layout(location = 0) in vec2 fragUv;
layout(location = 1) in vec3 fragWorldPosition;
layout(location = 2) in vec3 fragNormal;
layout(location = 3) in vec4 fragTangent;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform texture2D baseColorTexture;
layout(set = 0, binding = 4) uniform texture2D metallicRoughnessTexture;
layout(set = 0, binding = 5) uniform texture2D normalTexture;
layout(set = 0, binding = 6) uniform texture2D occlusionTexture;
layout(set = 0, binding = 7) uniform texture2D emissiveTexture;
layout(set = 0, binding = 8) uniform sampler materialSampler;

struct LightingConstants
{
    vec4 cameraPosition;
    vec4 directionalLightDirection;
    vec4 directionalLightColor;
    vec4 ambientColor;
    vec4 pbrParams;
    vec4 environmentColor;
};

layout(set = 0, binding = 1) uniform RendererLighting { LightingConstants lighting; };

layout(push_constant) uniform DrawConstants {
    mat4 viewProjectionMatrix;
    mat4 modelMatrix;
    uint textureFlags;
    uint padding0;
    uint padding1;
    uint padding2;
    vec4 emissiveColor;
    vec4 baseColor;
    vec4 materialParams;
} pushConstants;

const float PI = 3.14159265359;

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
    int textureFlags = int(pushConstants.textureFlags);
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
    vec3 tangentNormal = texture(sampler2D(normalTexture, materialSampler), fragUv).xyz * 2.0 - 1.0;
    tangentNormal.xy *= normalScale;
    return normalize(tbn * tangentNormal);
}

vec3 EvaluateDirectLight(
    vec3 normal,
    vec3 viewDir,
    vec3 lightDir,
    vec3 incidentIlluminance,
    vec3 albedo,
    float metallic,
    float roughness) {
    float ndotl = max(dot(normal, lightDir), 0.0);
    float ndotv = max(dot(normal, viewDir), 0.0);
    if (ndotl <= 0.0 || ndotv <= 0.0) {
        return vec3(0.0,0.0,0.0);
    }
    vec3 halfwayVector = viewDir + lightDir;
    float halfwayLengthSquared = dot(halfwayVector, halfwayVector);
    if (halfwayLengthSquared <= 0.000001) {
        return vec3(0.0,0.0,0.0);
    }
    vec3 halfway = halfwayVector * inversesqrt(halfwayLengthSquared);
    vec3 f0 = mix(vec3(0.04,0.04,0.04), albedo, metallic);
    float ndf = DistributionGGX(normal, halfway, roughness);
    float geometry = GeometrySmith(normal, viewDir, lightDir, roughness);
    vec3 fresnel = FresnelSchlick(max(dot(halfway, viewDir), 0.0), f0);
    vec3 specular = (ndf * geometry * fresnel) / max(4.0 * ndotv * ndotl, 0.0001);
    vec3 diffuseWeight = (vec3(1.0,1.0,1.0) - fresnel) * (1.0 - metallic);
    return (diffuseWeight * albedo / PI + specular) * incidentIlluminance * ndotl;
}

void main() {
    int textureFlags = int(pushConstants.textureFlags);
    vec3 albedo = pushConstants.baseColor.rgb;
    if ((textureFlags & 1) != 0) {
        albedo *= texture(sampler2D(baseColorTexture, materialSampler), fragUv).rgb;
    }

    float minRoughness = clamp(lighting.pbrParams.x, 0.01, 1.0);
    float ambientSpecularStrength = max(lighting.pbrParams.y, 0.0);
    float metallic = clamp(pushConstants.materialParams.x, 0.0, 1.0);
    float roughness = clamp(pushConstants.materialParams.y, minRoughness, 1.0);
    float occlusion = clamp(pushConstants.materialParams.w, 0.0, 1.0);
    if ((textureFlags & 2) != 0) {
        vec4 metallicRoughness = texture(sampler2D(metallicRoughnessTexture, materialSampler), fragUv);
        roughness = clamp(roughness * metallicRoughness.g, minRoughness, 1.0);
        metallic = clamp(metallic * metallicRoughness.b, 0.0, 1.0);
    }
    if ((textureFlags & 8) != 0) {
        occlusion *= texture(sampler2D(occlusionTexture, materialSampler), fragUv).r;
    }

    vec3 normal = GetNormal();
    vec3 viewDir = normalize(lighting.cameraPosition.xyz - fragWorldPosition);
    vec3 lightDir = normalize(lighting.directionalLightDirection.xyz);

    vec3 halfway = normalize(viewDir + lightDir);

    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 fresnel = FresnelSchlick(max(dot(halfway, viewDir), 0.0), f0);

    vec3 kS = fresnel;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);
    vec3 directLight = EvaluateDirectLight(normal, viewDir, lightDir,
        lighting.directionalLightColor.rgb * max(lighting.directionalLightColor.a, 0.0),
        albedo, metallic, roughness);
    vec3 ambientFresnel = FresnelSchlickRoughness(max(dot(normal, viewDir), 0.0), f0, roughness);
    vec3 ambientDiffuse = kD * albedo * lighting.ambientColor.rgb * lighting.ambientColor.a;
    vec3 ambientSpecular = ambientFresnel * lighting.environmentColor.rgb * lighting.environmentColor.a * ambientSpecularStrength;
    vec3 ambient = (ambientDiffuse + ambientSpecular) * occlusion;
    vec3 emissive = pushConstants.emissiveColor.rgb;
    if ((textureFlags & 16) != 0) {
        emissive *= texture(sampler2D(emissiveTexture, materialSampler), fragUv).rgb;
    }
    vec3 color = ambient + directLight + emissive;

    color = color / (color + vec3(1.0));
    if (lighting.pbrParams.z > 0.5) color = pow(color, vec3(1.0 / 2.2));
    outColor = vec4(color, pushConstants.baseColor.a);
}
