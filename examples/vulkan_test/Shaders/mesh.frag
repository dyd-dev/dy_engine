#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec3 fragWorldPosition;
layout(location = 3) in vec4 fragTangent;

layout(set = 0, binding = 0) uniform sampler2D baseColorTexture;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    mat4 model;
    float drawMode;
    uint firstIndex;
    int vertexOffset;
    uint firstVertex;
    float padding;
    vec4 baseColorFactor;
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

vec3 GetPerturbedNormal() {
    vec3 normal = normalize(fragNormal);
    vec3 tangent = normalize(fragTangent.xyz - normal * dot(normal, fragTangent.xyz));
    vec3 bitangent = normalize(cross(normal, tangent)) * fragTangent.w;
    mat3 tbn = mat3(tangent, bitangent, normal);

    float normalScale = pushConstants.materialParams.z;
    vec2 wave = vec2(
        sin(fragUV.x * 36.0 + fragUV.y * 9.0),
        cos(fragUV.y * 42.0 + fragUV.x * 7.0));
    vec3 tangentNormal = normalize(vec3(wave * normalScale, 1.0));
    return normalize(tbn * tangentNormal);
}

void main() {
    vec3 albedoSample = texture(baseColorTexture, fragUV * 2.0).rgb;
    vec3 albedo = mix(vec3(1.0), albedoSample, 0.18) * pushConstants.baseColorFactor.rgb;
    float metallic = clamp(pushConstants.materialParams.x, 0.0, 1.0);
    float roughness = clamp(pushConstants.materialParams.y, 0.04, 1.0);
    float exposure = max(pushConstants.materialParams.w, 0.001);

    vec3 normal = GetPerturbedNormal();
    vec3 viewDir = normalize(vec3(0.0, 0.0, 2.2) - fragWorldPosition);
    vec3 lightDir = normalize(vec3(0.35, 0.65, 0.68));
    vec3 halfway = normalize(viewDir + lightDir);
    vec3 radiance = vec3(4.0, 3.74, 3.28);

    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    float ndf = DistributionGGX(normal, halfway, roughness);
    float geometry = GeometrySmith(normal, viewDir, lightDir, roughness);
    vec3 fresnel = FresnelSchlick(max(dot(halfway, viewDir), 0.0), f0);

    vec3 numerator = ndf * geometry * fresnel;
    float denominator = max(4.0 * max(dot(normal, viewDir), 0.0) * max(dot(normal, lightDir), 0.0), 0.0001);
    vec3 specular = numerator / denominator;

    vec3 kS = fresnel;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);
    float ndotl = max(dot(normal, lightDir), 0.0);
    vec3 directLight = (kD * albedo / PI + specular) * radiance * ndotl;
    vec3 ambient = albedo * vec3(0.035);
    vec3 color = (ambient + directLight) * exposure;

    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0 / 2.2));
    outColor = vec4(color, pushConstants.baseColorFactor.a);
}
