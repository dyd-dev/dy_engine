#version 450
layout(location=0) in vec3 worldPosition;
layout(location=1) in vec3 worldNormal;
layout(location=0) out vec4 outColor;

layout(set=0,binding=1) uniform LightingConstants
{
    vec4 cameraPosition;
    vec4 directionalLightDirection;
    vec4 directionalLightColor;
    vec4 ambientColor;
    vec4 pbrParams;
    vec4 environmentColor;
} lighting;
layout(push_constant) uniform DrawConstants
{
    mat4 viewProjectionMatrix;
    mat4 modelMatrix;
    vec4 baseColor;
    float metallic;
    float roughness;
} draw;

const float PI = 3.14159265359;

float DistributionGGX(vec3 normal, vec3 halfway, float roughness)
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

float GeometrySmith(vec3 normal, vec3 viewDir, vec3 lightDir, float roughness)
{
    return GeometrySchlickGGX(max(dot(normal, viewDir), 0.0), roughness)
        * GeometrySchlickGGX(max(dot(normal, lightDir), 0.0), roughness);
}

vec3 FresnelSchlick(float cosTheta, vec3 f0)
{
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 FresnelSchlickRoughness(float cosTheta, vec3 f0, float roughness)
{
    vec3 roughnessF0 = max(vec3(1.0 - roughness), f0);
    return f0 + (roughnessF0 - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// 원래 PBR 수식으로 방향광 하나를 평가한다.
vec3 EvaluateDirectLight(vec3 normal, vec3 viewDir, vec3 lightDir,
    vec3 incidentIlluminance, vec3 albedo, float metallic, float roughness)
{
    float ndotl = max(dot(normal, lightDir), 0.0);
    float ndotv = max(dot(normal, viewDir), 0.0);
    if(ndotl <= 0.0 || ndotv <= 0.0) return vec3(0.0);
    vec3 halfwayVector = viewDir + lightDir;
    float halfwayLengthSquared = dot(halfwayVector, halfwayVector);
    if(halfwayLengthSquared <= 0.000001) return vec3(0.0);
    vec3 halfway = halfwayVector * inversesqrt(halfwayLengthSquared);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    float ndf = DistributionGGX(normal, halfway, roughness);
    float geometry = GeometrySmith(normal, viewDir, lightDir, roughness);
    vec3 fresnel = FresnelSchlick(max(dot(halfway, viewDir), 0.0), f0);
    vec3 specular = (ndf * geometry * fresnel) / max(4.0 * ndotv * ndotl, 0.0001);
    vec3 diffuseWeight = (vec3(1.0) - fresnel) * (1.0 - metallic);
    return (diffuseWeight * albedo / PI + specular) * incidentIlluminance * ndotl;
}

void main()
{
    vec3 albedo = draw.baseColor.rgb;

    float minRoughness = clamp(lighting.pbrParams.x, 0.01, 1.0);
    float metallic = clamp(draw.metallic, 0.0, 1.0);
    float roughness = clamp(draw.roughness, minRoughness, 1.0);
    vec3 normal = normalize(worldNormal);
    vec3 viewDir = normalize(lighting.cameraPosition.xyz - worldPosition);
    vec3 lightDir = normalize(lighting.directionalLightDirection.xyz);
    vec3 halfway = normalize(viewDir + lightDir);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 fresnel = FresnelSchlick(max(dot(halfway, viewDir), 0.0), f0);
    vec3 diffuseWeight = (vec3(1.0) - fresnel) * (1.0 - metallic);
    vec3 directLight = EvaluateDirectLight(normal, viewDir, lightDir,
        lighting.directionalLightColor.rgb * lighting.directionalLightColor.a,
        albedo, metallic, roughness);
    vec3 ambientFresnel = FresnelSchlickRoughness(max(dot(normal, viewDir), 0.0), f0, roughness);
    vec3 ambientDiffuse = diffuseWeight * albedo * lighting.ambientColor.rgb * lighting.ambientColor.a;
    vec3 ambientSpecular = ambientFresnel * lighting.environmentColor.rgb * lighting.environmentColor.a
        * max(lighting.pbrParams.y, 0.0);
    vec3 color = ambientDiffuse + ambientSpecular + directLight;
    color = color / (color + vec3(1.0));
    if(lighting.pbrParams.z > 0.5) color = pow(color, vec3(1.0 / 2.2));
    outColor = vec4(color, draw.baseColor.a);
}
