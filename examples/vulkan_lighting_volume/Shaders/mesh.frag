#version 450

layout(location = 0) in vec3 fragWorldPosition;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragUV;

layout(set = 0, binding = 1) uniform LightingVolumeProfile {
    vec4 globalLightDirection;
    vec4 globalLightColor;
    vec4 spotLightPosition;
    vec4 spotLightDirection;
    vec4 spotLightColor;
    vec4 volumeParams;
    vec4 volumeParams2;
} lighting;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 normal = normalize(fragNormal);
    vec3 baseColor = mix(vec3(0.22, 0.48, 0.78), vec3(0.92, 0.68, 0.28), fragUV.x);
    vec3 sunMarkerPosition = vec3(lighting.volumeParams2.y, lighting.volumeParams2.z, 0.0);
    if (length(fragWorldPosition - sunMarkerPosition) < 0.05) {
        vec3 markerColor = vec3(1.0, 0.74, 0.18) * (1.35 + lighting.globalLightColor.a * 0.35);
        outColor = vec4(markerColor / (markerColor + vec3(0.65)), 1.0);
        return;
    }

    float ambientIntensity = lighting.volumeParams.x;
    float exposure = lighting.volumeParams.y;
    float bloomLift = lighting.volumeParams.z;
    float spotInnerCos = lighting.volumeParams.w;
    float spotOuterCos = lighting.volumeParams2.x;

    vec3 globalDirection = normalize(-lighting.globalLightDirection.xyz);
    float rawGlobalNdotL = dot(normal, globalDirection);
    float globalMask = max(rawGlobalNdotL, 0.0);
    vec3 globalLight = lighting.globalLightColor.rgb * lighting.globalLightColor.a * globalMask;

    vec3 toSpot = lighting.spotLightPosition.xyz - fragWorldPosition;
    float spotDistance = max(length(toSpot), 0.001);
    vec3 spotL = toSpot / spotDistance;
    vec3 spotForward = normalize(lighting.spotLightDirection.xyz);
    float cone = dot(-spotL, spotForward);
    float coneAttenuation = smoothstep(spotOuterCos, spotInnerCos, cone);
    float distanceAttenuation = 1.0 / (1.0 + spotDistance * spotDistance * 2.5);
    float spotNdotL = max(dot(normal, spotL), 0.0);
    vec3 spotLight = lighting.spotLightColor.rgb * lighting.spotLightColor.a * spotNdotL * coneAttenuation * distanceAttenuation;

    vec3 color = baseColor * (vec3(ambientIntensity) + globalLight + spotLight);
    color *= exposure;

    float highlight = max(max(color.r, color.g), color.b);
    color += max(highlight - 1.0, 0.0) * bloomLift;
    color = color / (color + vec3(1.0));

    outColor = vec4(color, 1.0);
}
