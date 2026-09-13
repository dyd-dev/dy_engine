#version 450

layout(location = 1) in vec3 fragWorldPosition;
layout(location = 2) in vec3 fragNormal;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 2) uniform texture2D shadowMap;
layout(set = 0, binding = 9) uniform sampler shadowSampler;

	struct RendererDirectionalLight
	{
		vec4 directionIntensity;
		vec4 color;
	};

	struct RendererPointLight
	{
		vec4 positionRange;
		vec4 colorIntensity;
	};

	struct RendererSpotLight
	{
		vec4 positionRange;
		vec4 directionOuterCos;
		vec4 colorIntensity;
		vec4 coneParams;
	};

	struct RendererRectAreaLight
	{
		vec4 positionIntensity;
		vec4 directionWidth;
		vec4 upHeight;
		vec4 color;
	};

	struct RendererDiscAreaLight
	{
		vec4 positionIntensity;
		vec4 directionRadius;
		vec4 up;
		vec4 color;
	};

	struct LightingConstants
	{
		vec4 cameraPosition;
		vec4 directionalLightDirection;
		vec4 ambientColor;
		vec4 shadowParams;
		vec4 pbrParams;
		vec4 environmentColor;
		vec4 pointLightPositionRange;
		vec4 pointLightColorIntensity;
		vec4 lightCounts;
		vec4 areaLightCounts;
		RendererDirectionalLight directionalLights[1];
		RendererPointLight pointLights[1];
		RendererSpotLight spotLights[1];
		RendererRectAreaLight rectAreaLights[1];
		RendererDiscAreaLight discAreaLights[1];
	};

layout(set = 0, binding = 1) uniform RendererLighting { LightingConstants lighting; };

layout(push_constant) uniform DrawConstants {
    mat4 viewProjectionMatrix;
    mat4 modelMatrix;
    vec4 baseColor;
    float metallic;
    float roughness;
    uint receiveShadow;
    uint shadowViewIndex;
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

layout(set=0,binding=3) uniform ShadowMatrix {
// 정적 예제의 방향광 1개·점광원 6면·스폿 광원 1개를 같은 깊이 아틀라스에 저장한다.
mat4 lightViewProjectionMatrix[8];
vec4 atlasRect[8];
vec4 directionalViews[1];
vec4 pointViews[1];
vec4 spotViews[1];

} shadowMatrix;

float SampleShadow(int type,int lightIndex,vec3 worldPosition,vec3 normal,vec3 lightDir )
{
    if(pushConstants.receiveShadow==0u)return 1.0;
    vec4 views=type==1?shadowMatrix.directionalViews[lightIndex]:(type==2?shadowMatrix.pointViews[lightIndex]:shadowMatrix.spotViews[lightIndex]);
    int count=int(views.y+.5);
    if(count==0)return 1.0;
    int local=0;
    if(type==2)
    {
        vec3 direction=-lightDir;
        vec3 magnitude=abs(direction);
        if(magnitude.x>=magnitude.y && magnitude.x>=magnitude.z)local=direction.x>=0?0:1;
        else if(magnitude.y>=magnitude.z)local=direction.y>=0?2:3;
        else local=direction.z>=0?4:5;
    }
    int index=int(views.x+.5)+local;
    vec4 clip=shadowMatrix.lightViewProjectionMatrix[index]*vec4(worldPosition,1.0);
    if(clip.w<=0.0)return 1.0;
    vec3 ndc=clip.xyz/clip.w;
    vec2 uv=ndc.xy*.5+.5;
    uv.y=1.0-uv.y;
    if(uv.x<0.0||uv.y<0.0||uv.x>1.0||uv.y>1.0||ndc.z<0.0||ndc.z>1.0)return 1.0;
    vec4 tile=shadowMatrix.atlasRect[index];
    vec2 texel=1.0/vec2(textureSize(sampler2D(shadowMap,shadowSampler),0));
    vec2 minUv=tile.xy+texel*.5;
    vec2 maxUv=tile.xy+tile.zw-texel*.5;
    uv=tile.xy+uv*tile.zw;
    float angle=1.0-max(dot(normal,lightDir),0.0);
    float bias=max(lighting.shadowParams.x,lighting.shadowParams.y*angle)+lighting.shadowParams.z*angle;
    float receiver=ndc.z-bias;
    float radius=lighting.shadowParams.w;
    int extent=int(ceil(radius));
    float visible=0.0,samples=0.0;
    for(int y=-extent;y<=extent;++y)for(int x=-extent;x<=extent;++x)
    {
        vec2 sampleUv=clamp(uv+vec2(x,y)*texel,minUv,maxUv);
        visible+=receiver<=texture(sampler2D(shadowMap,shadowSampler),sampleUv).r?1.0:0.0;
        samples+=1.0;
    }
    return 1.0-clamp(views.z,0.0,1.0)*(1.0-visible/max(samples,1.0));
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
    float roughness,
    vec3 worldPosition) {
    vec3 toLight = samplePosition - worldPosition;
    float distanceSquared = dot(toLight, toLight);
    if (distanceSquared <= 0.000001 || sampleArea <= 0.0) {
        return vec3(0.0,0.0,0.0);
    }
    vec3 lightDir = toLight * inversesqrt(distanceSquared);
    float emitterCosine = max(dot(emitterDirection, -lightDir), 0.0);
    vec3 incidentIlluminance = emittedRadiance * emitterCosine * sampleArea / distanceSquared;
    return EvaluateDirectLight(normal, viewDir, lightDir, incidentIlluminance, albedo, metallic, roughness, 1.0);
}

vec3 EvaluateRectAreaLight(
    RendererRectAreaLight light,
    vec3 normal,
    vec3 viewDir,
    vec3 albedo,
    float metallic,
    float roughness,
    vec3 worldPosition) {
    vec3 emitterDirection = normalize(light.directionWidth.xyz);
    vec3 emitterUp = normalize(light.upHeight.xyz);
    vec3 emitterRight = normalize(cross(emitterDirection, emitterUp));
    emitterUp = normalize(cross(emitterRight, emitterDirection));
    float width = max(light.directionWidth.w, 0.0);
    float height = max(light.upHeight.w, 0.0);
    float sampleArea = width * height * 0.25;
    vec3 emittedRadiance = light.color.rgb * max(light.positionIntensity.w, 0.0);
    vec3 result = vec3(0.0,0.0,0.0);
    for (int sampleIndex = 0; sampleIndex < 4; ++sampleIndex) {
        float x = (sampleIndex & 1) == 0 ? -0.25 : 0.25;
        float y = (sampleIndex & 2) == 0 ? -0.25 : 0.25;
        vec3 samplePosition = light.positionIntensity.xyz + emitterRight * (x * width) + emitterUp * (y * height);
        result += EvaluateAreaSample(normal, viewDir, samplePosition, emitterDirection, emittedRadiance, sampleArea, albedo, metallic, roughness, worldPosition);
    }
    return result;
}

vec3 EvaluateDiscAreaLight(
    RendererDiscAreaLight light,
    vec3 normal,
    vec3 viewDir,
    vec3 albedo,
    float metallic,
    float roughness,
    vec3 worldPosition) {
    const int sampleCount = 8;
    const float goldenAngle = 2.39996323;
    vec3 emitterDirection = normalize(light.directionRadius.xyz);
    vec3 emitterUp = normalize(light.up.xyz);
    vec3 emitterRight = normalize(cross(emitterDirection, emitterUp));
    emitterUp = normalize(cross(emitterRight, emitterDirection));
    float radius = max(light.directionRadius.w, 0.0);
    float sampleArea = PI * radius * radius / float(sampleCount);
    vec3 emittedRadiance = light.color.rgb * max(light.positionIntensity.w, 0.0);
    vec3 result = vec3(0.0,0.0,0.0);
    for (int sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
        float normalizedRadius = sqrt((float(sampleIndex) + 0.5) / float(sampleCount));
        float angle = float(sampleIndex) * goldenAngle;
        vec2 discOffset = vec2(cos(angle), sin(angle)) * (normalizedRadius * radius);
        vec3 samplePosition = light.positionIntensity.xyz + emitterRight * discOffset.x + emitterUp * discOffset.y;
        result += EvaluateAreaSample(normal, viewDir, samplePosition, emitterDirection, emittedRadiance, sampleArea, albedo, metallic, roughness, worldPosition);
    }
    return result;
}

vec3 EvaluateLights(LightingConstants lighting, vec3 worldPosition, vec3 normal, vec3 viewDir,
    vec3 albedo, float metallic, float roughness) {
    vec3 directLight = vec3(0.0,0.0,0.0);
    int directionalCount = clamp(int(lighting.lightCounts.x + 0.5), 0, 1);
    for (int lightIndex = 0; lightIndex < directionalCount; ++lightIndex) {
        RendererDirectionalLight light = lighting.directionalLights[lightIndex];
        vec3 lightDir = normalize(light.directionIntensity.xyz);
        float visibility = SampleShadow(1,lightIndex,worldPosition,normal,lightDir);
        directLight += EvaluateDirectLight(normal, viewDir, lightDir,
            light.color.rgb * max(light.directionIntensity.w, 0.0), albedo, metallic, roughness, visibility);
    }
    int pointCount = clamp(int(lighting.lightCounts.y + 0.5), 0, 1);
    for (int lightIndex = 0; lightIndex < pointCount; ++lightIndex) {
        RendererPointLight light = lighting.pointLights[lightIndex];
        vec3 toLight = light.positionRange.xyz - worldPosition;
        float distanceToLight = length(toLight);
        if (distanceToLight <= 0.0001) {
            continue;
        }
        vec3 lightDir = toLight / distanceToLight;
        float visibility = SampleShadow(2,lightIndex,worldPosition,normal,lightDir);
        vec3 incidentIlluminance = light.colorIntensity.rgb * max(light.colorIntensity.w, 0.0) *
            PunctualAttenuation(distanceToLight, light.positionRange.w);
        directLight += EvaluateDirectLight(normal, viewDir, lightDir, incidentIlluminance, albedo, metallic, roughness, visibility);
    }
    int spotCount = clamp(int(lighting.lightCounts.z + 0.5), 0, 1);
    for (int lightIndex = 0; lightIndex < spotCount; ++lightIndex) {
        RendererSpotLight light = lighting.spotLights[lightIndex];
        vec3 toLight = light.positionRange.xyz - worldPosition;
        float distanceToLight = length(toLight);
        if (distanceToLight <= 0.0001) {
            continue;
        }
        vec3 lightDir = toLight / distanceToLight;
        float attenuation = PunctualAttenuation(distanceToLight, light.positionRange.w) *
            SpotConeAttenuation(dot(-lightDir, normalize(light.directionOuterCos.xyz)),
                light.coneParams.x, light.directionOuterCos.w);
        vec3 incidentIlluminance = light.colorIntensity.rgb * max(light.colorIntensity.w, 0.0) * attenuation;
        float visibility = SampleShadow(3,lightIndex,worldPosition,normal,lightDir);
        directLight += EvaluateDirectLight(normal, viewDir, lightDir, incidentIlluminance, albedo, metallic, roughness, visibility);
    }
    int rectAreaCount = clamp(int(lighting.areaLightCounts.x + 0.5), 0, 1);
    for (int lightIndex = 0; lightIndex < rectAreaCount; ++lightIndex) {
        directLight += EvaluateRectAreaLight(lighting.rectAreaLights[lightIndex], normal, viewDir, albedo, metallic, roughness, worldPosition);
    }
    int discAreaCount = clamp(int(lighting.areaLightCounts.y + 0.5), 0, 1);
    for (int lightIndex = 0; lightIndex < discAreaCount; ++lightIndex) {
        directLight += EvaluateDiscAreaLight(lighting.discAreaLights[lightIndex], normal, viewDir, albedo, metallic, roughness, worldPosition);
    }

    return directLight;
}

void main() {
    vec3 albedo = pushConstants.baseColor.rgb;

    float minRoughness = clamp(lighting.pbrParams.x, 0.01, 1.0);
    float ambientSpecularStrength = max(lighting.pbrParams.y, 0.0);
    float metallic = clamp(pushConstants.metallic, 0.0, 1.0);
    float roughness = clamp(pushConstants.roughness, minRoughness, 1.0);

    vec3 normal = normalize(fragNormal);
    vec3 viewDir = normalize(lighting.cameraPosition.xyz - fragWorldPosition);
    bool usePointLight = lighting.pointLightColorIntensity.a > 0.0 && lighting.pointLightPositionRange.w > 0.0;
    vec3 lightDir = normalize(lighting.directionalLightDirection.xyz);
    if (usePointLight) {
        vec3 toLight = lighting.pointLightPositionRange.xyz - fragWorldPosition;
        float distanceToLight = length(toLight);
        lightDir = distanceToLight > 0.0001 ? toLight / distanceToLight : vec3(0.0, 0.0, 1.0);
    }
    vec3 halfway = normalize(viewDir + lightDir);

    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 fresnel = FresnelSchlick(max(dot(halfway, viewDir), 0.0), f0);

    vec3 kS = fresnel;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);
    vec3 directLight = EvaluateLights(lighting, fragWorldPosition, normal, viewDir, albedo, metallic, roughness);
    vec3 ambientFresnel = FresnelSchlickRoughness(max(dot(normal, viewDir), 0.0), f0, roughness);
    vec3 ambientDiffuse = kD * albedo * lighting.ambientColor.rgb * lighting.ambientColor.a;
    vec3 ambientSpecular = ambientFresnel * lighting.environmentColor.rgb * lighting.environmentColor.a * ambientSpecularStrength;
    vec3 ambient = (ambientDiffuse + ambientSpecular);

    vec3 color = lighting.lightCounts.w > 0.5 ? ambient + directLight : albedo;

    if(lighting.pbrParams.z >= 0.0) {
        color *= lighting.pbrParams.w;
    color = color / (color + vec3(1.0));
    if (lighting.pbrParams.z > 0.5) color = pow(color, vec3(1.0 / 2.2));
    }
    outColor = vec4(color, pushConstants.baseColor.a);
}
