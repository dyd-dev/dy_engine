DY_LIGHT_INLINE float PunctualAttenuation(float distanceToLight, float range) {
    if (range <= 0.0 || distanceToLight >= range) {
        return 0.0;
    }
    float distanceSquared = max(distanceToLight * distanceToLight, 0.0001);
    float ratio = distanceToLight / range;
    float ratioSquared = ratio * ratio;
    float window = max(1.0 - ratioSquared * ratioSquared, 0.0);
    return (window * window) / distanceSquared;
}

DY_LIGHT_INLINE float SpotConeAttenuation(float cosTheta, float innerCos, float outerCos) {
    float denominator = max(innerCos - outerCos, 0.0001);
    float t = clamp((cosTheta - outerCos) / denominator, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

DY_LIGHT_INLINE float3 EvaluateDirectLight(
    float3 normal,
    float3 viewDir,
    float3 lightDir,
    float3 incidentIlluminance,
    float3 albedo,
    float metallic,
    float roughness,
    float visibility) {
    float ndotl = max(dot(normal, lightDir), 0.0);
    float ndotv = max(dot(normal, viewDir), 0.0);
    if (ndotl <= 0.0 || ndotv <= 0.0 || visibility <= 0.0) {
        return float3(0.0,0.0,0.0);
    }
    float3 halfwayVector = viewDir + lightDir;
    float halfwayLengthSquared = dot(halfwayVector, halfwayVector);
    if (halfwayLengthSquared <= 0.000001) {
        return float3(0.0,0.0,0.0);
    }
    float3 halfway = halfwayVector * DY_LIGHT_RSQRT(halfwayLengthSquared);
    float3 f0 = DY_LIGHT_MIX(float3(0.04,0.04,0.04), albedo, metallic);
    float ndf = DistributionGGX(normal, halfway, roughness);
    float geometry = GeometrySmith(normal, viewDir, lightDir, roughness);
    float3 fresnel = FresnelSchlick(max(dot(halfway, viewDir), 0.0), f0);
    float3 specular = (ndf * geometry * fresnel) / max(4.0 * ndotv * ndotl, 0.0001);
    float3 diffuseWeight = (float3(1.0,1.0,1.0) - fresnel) * (1.0 - metallic);
    return (diffuseWeight * albedo / DY_LIGHT_PI + specular) * incidentIlluminance * ndotl * visibility;
}

DY_LIGHT_INLINE float3 EvaluateAreaSample(
    float3 normal,
    float3 viewDir,
    float3 samplePosition,
    float3 emitterDirection,
    float3 emittedRadiance,
    float sampleArea,
    float3 albedo,
    float metallic,
    float roughness,
    float3 worldPosition) {
    float3 toLight = samplePosition - worldPosition;
    float distanceSquared = dot(toLight, toLight);
    if (distanceSquared <= 0.000001 || sampleArea <= 0.0) {
        return float3(0.0,0.0,0.0);
    }
    float3 lightDir = toLight * DY_LIGHT_RSQRT(distanceSquared);
    float emitterCosine = max(dot(emitterDirection, -lightDir), 0.0);
    float3 incidentIlluminance = emittedRadiance * emitterCosine * sampleArea / distanceSquared;
    return EvaluateDirectLight(normal, viewDir, lightDir, incidentIlluminance, albedo, metallic, roughness, 1.0);
}

DY_LIGHT_INLINE float3 EvaluateRectAreaLight(
    RendererRectAreaLight light,
    float3 normal,
    float3 viewDir,
    float3 albedo,
    float metallic,
    float roughness,
    float3 worldPosition) {
    float3 emitterDirection = normalize(light.directionWidth.xyz);
    float3 emitterUp = normalize(light.upHeight.xyz);
    float3 emitterRight = normalize(cross(emitterDirection, emitterUp));
    emitterUp = normalize(cross(emitterRight, emitterDirection));
    float width = max(light.directionWidth.w, 0.0);
    float height = max(light.upHeight.w, 0.0);
    float sampleArea = width * height * 0.25;
    float3 emittedRadiance = light.color.rgb * max(light.positionIntensity.w, 0.0);
    float3 result = float3(0.0,0.0,0.0);
    for (int sampleIndex = 0; sampleIndex < 4; ++sampleIndex) {
        float x = (sampleIndex & 1) == 0 ? -0.25 : 0.25;
        float y = (sampleIndex & 2) == 0 ? -0.25 : 0.25;
        float3 samplePosition = light.positionIntensity.xyz + emitterRight * (x * width) + emitterUp * (y * height);
        result += EvaluateAreaSample(normal, viewDir, samplePosition, emitterDirection, emittedRadiance, sampleArea, albedo, metallic, roughness, worldPosition);
    }
    return result;
}

DY_LIGHT_INLINE float3 EvaluateDiscAreaLight(
    RendererDiscAreaLight light,
    float3 normal,
    float3 viewDir,
    float3 albedo,
    float metallic,
    float roughness,
    float3 worldPosition) {
    const int sampleCount = 8;
    const float goldenAngle = 2.39996323;
    float3 emitterDirection = normalize(light.directionRadius.xyz);
    float3 emitterUp = normalize(light.up.xyz);
    float3 emitterRight = normalize(cross(emitterDirection, emitterUp));
    emitterUp = normalize(cross(emitterRight, emitterDirection));
    float radius = max(light.directionRadius.w, 0.0);
    float sampleArea = DY_LIGHT_PI * radius * radius / float(sampleCount);
    float3 emittedRadiance = light.color.rgb * max(light.positionIntensity.w, 0.0);
    float3 result = float3(0.0,0.0,0.0);
    for (int sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
        float normalizedRadius = sqrt((float(sampleIndex) + 0.5) / float(sampleCount));
        float angle = float(sampleIndex) * goldenAngle;
        float2 discOffset = float2(cos(angle), sin(angle)) * (normalizedRadius * radius);
        float3 samplePosition = light.positionIntensity.xyz + emitterRight * discOffset.x + emitterUp * discOffset.y;
        result += EvaluateAreaSample(normal, viewDir, samplePosition, emitterDirection, emittedRadiance, sampleArea, albedo, metallic, roughness, worldPosition);
    }
    return result;
}

DY_LIGHT_INLINE float3 EvaluateLights(DY_LIGHT_ARGUMENT, float3 worldPosition, float3 normal, float3 viewDir,
    float3 albedo, float metallic, float roughness, float shadowVisibility) {
    int shadowType = int(lighting.shadowLight.x + 0.5);
    int shadowIndex = int(lighting.shadowLight.y + 0.5);
    float3 directLight = float3(0.0,0.0,0.0);
    int directionalCount = clamp(int(lighting.lightCounts.x + 0.5), 0, RENDERER_MAX_DIRECTIONAL_LIGHTS);
    for (int lightIndex = 0; lightIndex < directionalCount; ++lightIndex) {
        RendererDirectionalLight light = lighting.directionalLights[lightIndex];
        float3 lightDir = normalize(light.directionIntensity.xyz);
        float visibility = shadowType == 1 && shadowIndex == lightIndex
            ? shadowVisibility
            : 1.0;
        directLight += EvaluateDirectLight(normal, viewDir, lightDir,
            light.color.rgb * max(light.directionIntensity.w, 0.0), albedo, metallic, roughness, visibility);
    }
    int pointCount = clamp(int(lighting.lightCounts.y + 0.5), 0, RENDERER_MAX_POINT_LIGHTS);
    for (int lightIndex = 0; lightIndex < pointCount; ++lightIndex) {
        RendererPointLight light = lighting.pointLights[lightIndex];
        float3 toLight = light.positionRange.xyz - worldPosition;
        float distanceToLight = length(toLight);
        if (distanceToLight <= 0.0001) {
            continue;
        }
        float3 lightDir = toLight / distanceToLight;
        float visibility = shadowType == 2 && shadowIndex == lightIndex
            ? shadowVisibility
            : 1.0;
        float3 incidentIlluminance = light.colorIntensity.rgb * max(light.colorIntensity.w, 0.0) *
            PunctualAttenuation(distanceToLight, light.positionRange.w);
        directLight += EvaluateDirectLight(normal, viewDir, lightDir, incidentIlluminance, albedo, metallic, roughness, visibility);
    }
    int spotCount = clamp(int(lighting.lightCounts.z + 0.5), 0, RENDERER_MAX_SPOT_LIGHTS);
    for (int lightIndex = 0; lightIndex < spotCount; ++lightIndex) {
        RendererSpotLight light = lighting.spotLights[lightIndex];
        float3 toLight = light.positionRange.xyz - worldPosition;
        float distanceToLight = length(toLight);
        if (distanceToLight <= 0.0001) {
            continue;
        }
        float3 lightDir = toLight / distanceToLight;
        float attenuation = PunctualAttenuation(distanceToLight, light.positionRange.w) *
            SpotConeAttenuation(dot(-lightDir, normalize(light.directionOuterCos.xyz)),
                light.coneParams.x, light.directionOuterCos.w);
        float3 incidentIlluminance = light.colorIntensity.rgb * max(light.colorIntensity.w, 0.0) * attenuation;
        float visibility = shadowType == 3 && shadowIndex == lightIndex
            ? shadowVisibility
            : 1.0;
        directLight += EvaluateDirectLight(normal, viewDir, lightDir, incidentIlluminance, albedo, metallic, roughness, visibility);
    }
    int rectAreaCount = clamp(int(lighting.areaLightCounts.x + 0.5), 0, RENDERER_MAX_RECT_AREA_LIGHTS);
    for (int lightIndex = 0; lightIndex < rectAreaCount; ++lightIndex) {
        directLight += EvaluateRectAreaLight(lighting.rectAreaLights[lightIndex], normal, viewDir, albedo, metallic, roughness, worldPosition);
    }
    int discAreaCount = clamp(int(lighting.areaLightCounts.y + 0.5), 0, RENDERER_MAX_DISC_AREA_LIGHTS);
    for (int lightIndex = 0; lightIndex < discAreaCount; ++lightIndex) {
        directLight += EvaluateDiscAreaLight(lighting.discAreaLights[lightIndex], normal, viewDir, albedo, metallic, roughness, worldPosition);
    }

    return directLight;
}
