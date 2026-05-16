#version 450

layout(location = 0) in vec3 fragWorldPosition;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragUV;
layout(location = 3) in float fragDrawMode;
layout(location = 4) in vec4 fragLightSpacePos;

layout(set = 0, binding = 1) uniform LightingVolumeProfile {
    vec4 globalLightDirection;
    vec4 globalLightColor;
    vec4 spotLightPosition;
    vec4 spotLightDirection;
    vec4 spotLightColor;
    vec4 volumeParams;
    vec4 volumeParams2;
    vec4 cameraPosition;
    vec4 materialParams;
} lighting;

// Shadow Map (Directional Light용 깊이 텍스처).
// The selected RHI backend binds this at binding=2.
// frustum 밖 픽셀은 그림자 없는 것으로 처리됨.
layout(set = 0, binding = 2) uniform sampler2D shadowMap;

layout(location = 0) out vec4 outColor;

float CalculateDirectionalShadow(vec4 lightSpacePos, vec3 normal, vec3 lightDir) {
    vec3 ndc = lightSpacePos.xyz / lightSpacePos.w;
    vec2 shadowUV = ndc.xy * 0.5 + 0.5;

    if (shadowUV.x < 0.0 || shadowUV.x > 1.0 ||
        shadowUV.y < 0.0 || shadowUV.y > 1.0 ||
        ndc.z < 0.0 || ndc.z > 1.0) {
        return 1.0;
    }

    float NdotL = max(dot(normal, lightDir), 0.0);
    float bias = max(0.005 * (1.0 - NdotL), 0.0008);
    float currentDepth = ndc.z;

    // 3x3 PCF
    float shadow = 0.0;
    vec2 texelSize = 1.0 / vec2(textureSize(shadowMap, 0));
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float pcfDepth = texture(shadowMap, shadowUV + vec2(x, y) * texelSize).r;
            shadow += (currentDepth - bias > pcfDepth) ? 0.0 : 1.0;
        }
    }
    return shadow / 9.0;
}

void main() {
    if (fragDrawMode > 3.5) {
        vec3 markerColor = vec3(0.18, 0.58, 1.0) * 2.1;
        outColor = vec4(markerColor / (markerColor + vec3(0.45)), 1.0);
        return;
    }

    if (fragDrawMode > 2.5) {
        vec3 markerColor = vec3(1.0, 0.58, 0.18) * 2.0;
        outColor = vec4(markerColor / (markerColor + vec3(0.55)), 1.0);
        return;
    }

    if (fragDrawMode > 1.5) {
        vec2 grid = abs(fract(fragUV * vec2(10.0, 8.0)) - 0.5);
        float gridLine = 1.0 - smoothstep(0.46, 0.50, min(grid.x, grid.y));
        vec3 floorColor = mix(vec3(0.12, 0.15, 0.17), vec3(0.20, 0.24, 0.23), fragUV.y);
        floorColor += gridLine * vec3(0.035, 0.04, 0.04);

        // 바닥은 ShadowMap 그림자도 받도록 처리.
        vec3 normal = vec3(0.0, 0.0, 1.0);
        vec3 globalDirection = normalize(-lighting.globalLightDirection.xyz);
        float shadowFactor = CalculateDirectionalShadow(fragLightSpacePos, normal, globalDirection);
        floorColor *= mix(0.55, 1.0, shadowFactor);

        outColor = vec4(floorColor, 1.0);
        return;
    }

    if (fragDrawMode < 0.0) {
        // Planar Projected Shadow (검은 반투명) 
        float daylight = clamp(-lighting.globalLightDirection.z, 0.0, 1.0);
        float alpha = mix(0.34, 0.16, daylight);
        outColor = vec4(0.0, 0.0, 0.0, alpha);
        return;
    }

    vec3 normal = normalize(fragNormal);
    vec3 baseColor = mix(vec3(0.22, 0.48, 0.78), vec3(0.92, 0.68, 0.28), fragUV.x);
    vec3 sunMarkerPosition = lighting.volumeParams2.yzw;
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
    float specularStrength = lighting.materialParams.x;
    float shininess = lighting.materialParams.y;
    vec3 viewDirection = normalize(lighting.cameraPosition.xyz - fragWorldPosition);

    vec3 globalDirection = normalize(-lighting.globalLightDirection.xyz);
    float rawGlobalNdotL = dot(normal, globalDirection);
    float globalMask = max(rawGlobalNdotL, 0.0);

    vec3 globalDiffuse = 
        baseColor * 
        lighting.globalLightColor.rgb * 
        lighting.globalLightColor.a * 
        globalMask;

    vec3 globalReflectDirection = reflect(-globalDirection, normal);

    float globalSpecularMask = 
        pow(max(dot(viewDirection, globalReflectDirection), 0.0), shininess) * 
        step(0.0, rawGlobalNdotL);
    vec3 globalSpecular = 
        lighting.globalLightColor.rgb * 
        lighting.globalLightColor.a * 
        specularStrength * 
        globalSpecularMask;

    // Presentation mode: objects cast shadows, while only the floor receives the shadow map.

    vec3 toSpot = lighting.spotLightPosition.xyz - fragWorldPosition;
    float spotDistance = max(length(toSpot), 0.001);
    vec3 spotL = toSpot / spotDistance;
    vec3 spotForward = normalize(lighting.spotLightDirection.xyz);
    float cone = dot(-spotL, spotForward);
    float coneAttenuation = smoothstep(spotOuterCos, spotInnerCos, cone);
    float distanceAttenuation = 1.0 / (1.0 + spotDistance * spotDistance * 2.5);
    float rawSpotNdotL = dot(normal, spotL);
    float spotNdotL = max(rawSpotNdotL, 0.0);
    float spotAttenuation = coneAttenuation * distanceAttenuation;
    vec3 spotDiffuse = baseColor * lighting.spotLightColor.rgb * lighting.spotLightColor.a * spotNdotL * spotAttenuation;
    vec3 spotReflectDirection = reflect(-spotL, normal);
    float spotSpecularMask = pow(max(dot(viewDirection, spotReflectDirection), 0.0), shininess) * step(0.0, rawSpotNdotL);
    vec3 spotSpecular = lighting.spotLightColor.rgb * lighting.spotLightColor.a * specularStrength * spotSpecularMask * spotAttenuation;

    vec3 color = baseColor * ambientIntensity + globalDiffuse + globalSpecular + spotDiffuse + spotSpecular;
    color *= exposure;

    float highlight = max(max(color.r, color.g), color.b);
    color += max(highlight - 1.0, 0.0) * bloomLift;
    color = color / (color + vec3(1.0));

    outColor = vec4(color, 1.0);
}
