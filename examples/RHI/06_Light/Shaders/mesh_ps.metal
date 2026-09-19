#include <metal_stdlib>

using namespace metal;

struct DrawConstants
{
    float4x4 viewProjectionMatrix;
    float4x4 modelMatrix;
    float4 baseColor;
    float metallic;
    float roughness;
    uint receiveShadow;
    uint shadowViewIndex;
};

	struct RendererDirectionalLight
	{
		float4 directionIntensity;
		float4 color;
	};

	struct RendererPointLight
	{
		float4 positionRange;
		float4 colorIntensity;
	};

	struct RendererSpotLight
	{
		float4 positionRange;
		float4 directionOuterCos;
		float4 colorIntensity;
		float4 coneParams;
	};

	struct RendererRectAreaLight
	{
		float4 positionIntensity;
		float4 directionWidth;
		float4 upHeight;
		float4 color;
	};

	struct RendererDiscAreaLight
	{
		float4 positionIntensity;
		float4 directionRadius;
		float4 up;
		float4 color;
	};

	struct LightingConstants
	{
		float4 cameraPosition;
		float4 directionalLightDirection;
		float4 ambientColor;
		float4 shadowParams;
		float4 pbrParams;
		float4 environmentColor;
		float4 pointLightPositionRange;
		float4 pointLightColorIntensity;
		float4 lightCounts;
		float4 areaLightCounts;
		RendererDirectionalLight directionalLights[1];
		RendererPointLight pointLights[1];
		RendererSpotLight spotLights[1];
		RendererRectAreaLight rectAreaLights[1];
		RendererDiscAreaLight discAreaLights[1];
	};

struct RasterData
{
    float4 position [[position]];
    float3 worldPosition [[user(locn1)]];
    float3 worldNormal [[user(locn2)]];
};

constant float kPi = 3.14159265359f;

inline float DistributionGGX(float3 normal, float3 halfway, float roughness)
{
    const float a = roughness * roughness;
    const float a2 = a * a;
    const float ndoth = max(dot(normal, halfway), 0.0f);
    const float ndoth2 = ndoth * ndoth;
    const float denominator = ndoth2 * (a2 - 1.0f) + 1.0f;
    return a2 / max(kPi * denominator * denominator, 0.0001f);
}

inline float GeometrySchlickGGX(float ndotv, float roughness)
{
    const float r = roughness + 1.0f;
    const float k = (r * r) / 8.0f;
    return ndotv / max(ndotv * (1.0f - k) + k, 0.0001f);
}

inline float GeometrySmith(
    float3 normal,
    float3 viewDirection,
    float3 lightDirection,
    float roughness)
{
    const float ndotv = max(dot(normal, viewDirection), 0.0f);
    const float ndotl = max(dot(normal, lightDirection), 0.0f);
    return GeometrySchlickGGX(ndotv, roughness) *
        GeometrySchlickGGX(ndotl, roughness);
}

inline float3 FresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0f - f0) *
        pow(saturate(1.0f - cosTheta), 5.0f);
}

inline float3 FresnelSchlickRoughness(
    float cosTheta,
    float3 f0,
    float roughness)
{
    const float3 roughnessF0 = max(float3(1.0f - roughness), f0);
    return f0 + (roughnessF0 - f0) *
        pow(saturate(1.0f - cosTheta), 5.0f);
}

struct ShadowMatrix {
// 정적 예제의 방향광 1개·점광원 6면·스폿 광원 1개를 같은 깊이 아틀라스에 저장한다.
float4x4 lightViewProjectionMatrix[8];
float4 atlasRect[8];
float4 directionalViews[1];
float4 pointViews[1];
float4 spotViews[1];

};

float SampleShadow(int type,int lightIndex,float3 worldPosition,float3 normal,float3 lightDir ,constant LightingConstants& lighting,constant ShadowMatrix& shadowMatrix,depth2d<float> shadowMap,sampler shadowSampler,uint drawFlags)
{
    if(drawFlags==0u)return 1.0;
    float4 views=type==1?shadowMatrix.directionalViews[lightIndex]:(type==2?shadowMatrix.pointViews[lightIndex]:shadowMatrix.spotViews[lightIndex]);
    int count=int(views.y+.5);
    if(count==0)return 1.0;
    int local=0;
    if(type==2)
    {
        float3 direction=-lightDir;
        float3 magnitude=abs(direction);
        if(magnitude.x>=magnitude.y && magnitude.x>=magnitude.z)local=direction.x>=0?0:1;
        else if(magnitude.y>=magnitude.z)local=direction.y>=0?2:3;
        else local=direction.z>=0?4:5;
    }
    int index=int(views.x+.5)+local;
    float4 clip=shadowMatrix.lightViewProjectionMatrix[index]*float4(worldPosition,1.0);
    if(clip.w<=0.0)return 1.0;
    float3 ndc=clip.xyz/clip.w;
    float2 uv=ndc.xy*.5+.5;
    uv.y=1.0-uv.y;
    if(uv.x<0.0||uv.y<0.0||uv.x>1.0||uv.y>1.0||ndc.z<0.0||ndc.z>1.0)return 1.0;
    float4 tile=shadowMatrix.atlasRect[index];
    float2 texel=1.0/float2(shadowMap.get_width(),shadowMap.get_height());
    float2 minUv=tile.xy+texel*.5;
    float2 maxUv=tile.xy+tile.zw-texel*.5;
    uv=tile.xy+uv*tile.zw;
    float angle=1.0-max(dot(normal,lightDir),0.0);
    float bias=max(lighting.shadowParams.x,lighting.shadowParams.y*angle)+lighting.shadowParams.z*angle;
    float receiver=ndc.z-bias;
    float radius=lighting.shadowParams.w;
    int extent=int(ceil(radius));
    float visible=0.0,samples=0.0;
    for(int y=-extent;y<=extent;++y)for(int x=-extent;x<=extent;++x)
    {
        float2 sampleUv=clamp(uv+float2(x,y)*texel,minUv,maxUv);
        visible+=receiver<=shadowMap.sample(shadowSampler,sampleUv)?1.0:0.0;
        samples+=1.0;
    }
    return 1.0-clamp(views.z,0.0,1.0)*(1.0-visible/max(samples,1.0));
}

inline float PunctualAttenuation(float distanceToLight, float range) {
    if (range <= 0.0 || distanceToLight >= range) {
        return 0.0;
    }
    float distanceSquared = max(distanceToLight * distanceToLight, 0.0001);
    float ratio = distanceToLight / range;
    float ratioSquared = ratio * ratio;
    float window = max(1.0 - ratioSquared * ratioSquared, 0.0);
    return (window * window) / distanceSquared;
}

inline float SpotConeAttenuation(float cosTheta, float innerCos, float outerCos) {
    float denominator = max(innerCos - outerCos, 0.0001);
    float t = clamp((cosTheta - outerCos) / denominator, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

inline float3 EvaluateDirectLight(
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
    float3 halfway = halfwayVector * rsqrt(halfwayLengthSquared);
    float3 f0 = mix(float3(0.04,0.04,0.04), albedo, metallic);
    float ndf = DistributionGGX(normal, halfway, roughness);
    float geometry = GeometrySmith(normal, viewDir, lightDir, roughness);
    float3 fresnel = FresnelSchlick(max(dot(halfway, viewDir), 0.0), f0);
    float3 specular = (ndf * geometry * fresnel) / max(4.0 * ndotv * ndotl, 0.0001);
    float3 diffuseWeight = (float3(1.0,1.0,1.0) - fresnel) * (1.0 - metallic);
    return (diffuseWeight * albedo / kPi + specular) * incidentIlluminance * ndotl * visibility;
}

inline float3 EvaluateAreaSample(
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
    float3 lightDir = toLight * rsqrt(distanceSquared);
    float emitterCosine = max(dot(emitterDirection, -lightDir), 0.0);
    float3 incidentIlluminance = emittedRadiance * emitterCosine * sampleArea / distanceSquared;
    return EvaluateDirectLight(normal, viewDir, lightDir, incidentIlluminance, albedo, metallic, roughness, 1.0);
}

inline float3 EvaluateRectAreaLight(
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

inline float3 EvaluateDiscAreaLight(
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
    float sampleArea = kPi * radius * radius / float(sampleCount);
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

inline float3 EvaluateLights(constant LightingConstants& lighting,constant ShadowMatrix& shadowMatrix,depth2d<float> shadowMap,sampler shadowSampler,uint drawFlags, float3 worldPosition, float3 normal, float3 viewDir,
    float3 albedo, float metallic, float roughness) {
    float3 directLight = float3(0.0,0.0,0.0);
    int directionalCount = clamp(int(lighting.lightCounts.x + 0.5), 0, 1);
    for (int lightIndex = 0; lightIndex < directionalCount; ++lightIndex) {
        RendererDirectionalLight light = lighting.directionalLights[lightIndex];
        float3 lightDir = normalize(light.directionIntensity.xyz);
        float visibility = SampleShadow(1,lightIndex,worldPosition,normal,lightDir,lighting,shadowMatrix,shadowMap,shadowSampler,drawFlags);
        directLight += EvaluateDirectLight(normal, viewDir, lightDir,
            light.color.rgb * max(light.directionIntensity.w, 0.0), albedo, metallic, roughness, visibility);
    }
    int pointCount = clamp(int(lighting.lightCounts.y + 0.5), 0, 1);
    for (int lightIndex = 0; lightIndex < pointCount; ++lightIndex) {
        RendererPointLight light = lighting.pointLights[lightIndex];
        float3 toLight = light.positionRange.xyz - worldPosition;
        float distanceToLight = length(toLight);
        if (distanceToLight <= 0.0001) {
            continue;
        }
        float3 lightDir = toLight / distanceToLight;
        float visibility = SampleShadow(2,lightIndex,worldPosition,normal,lightDir,lighting,shadowMatrix,shadowMap,shadowSampler,drawFlags);
        float3 incidentIlluminance = light.colorIntensity.rgb * max(light.colorIntensity.w, 0.0) *
            PunctualAttenuation(distanceToLight, light.positionRange.w);
        directLight += EvaluateDirectLight(normal, viewDir, lightDir, incidentIlluminance, albedo, metallic, roughness, visibility);
    }
    int spotCount = clamp(int(lighting.lightCounts.z + 0.5), 0, 1);
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
        float visibility = SampleShadow(3,lightIndex,worldPosition,normal,lightDir,lighting,shadowMatrix,shadowMap,shadowSampler,drawFlags);
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

fragment float4 fragmentMain(
    RasterData input [[stage_in]],
    constant DrawConstants& drawConstants [[buffer(10)]],
    constant LightingConstants& lighting [[buffer(1)]],
    depth2d<float> shadowMap [[texture(2)]],
    constant ShadowMatrix& shadowMatrix [[buffer(3)]],
    sampler shadowSampler [[sampler(9)]])
{
    float3 albedo = drawConstants.baseColor.rgb;

    const float minRoughness =
        clamp(lighting.pbrParams.x, 0.01f, 1.0f);
    const float ambientSpecularStrength =
        max(lighting.pbrParams.y, 0.0f);
    float metallic =
        clamp(drawConstants.metallic, 0.0f, 1.0f);
    float roughness =
        clamp(drawConstants.roughness, minRoughness, 1.0f);

    const float3 normal =
        normalize(input.worldNormal);
    const float3 viewDirection =
        normalize(lighting.cameraPosition.xyz - input.worldPosition);

    const bool usePointLight =
        lighting.pointLightColorIntensity.a > 0.0f &&
        lighting.pointLightPositionRange.w > 0.0f;
    float3 lightDirection =
        normalize(lighting.directionalLightDirection.xyz);

    if (usePointLight)
    {
        const float3 toLight =
            lighting.pointLightPositionRange.xyz - input.worldPosition;
        const float distanceToLight = length(toLight);
        lightDirection = distanceToLight > 0.0001f
            ? toLight / distanceToLight
            : float3(0.0f, 0.0f, 1.0f);
    }

    const float3 halfway =
        normalize(viewDirection + lightDirection);
    const float3 f0 =
        mix(float3(0.04f), albedo, metallic);
    const float3 fresnel =
        FresnelSchlick(max(dot(halfway, viewDirection), 0.0f), f0);

    const float3 kS = fresnel;
    const float3 kD =
        (float3(1.0f) - kS) * (1.0f - metallic);
    const float3 directLight = EvaluateLights(lighting,
        shadowMatrix,shadowMap,shadowSampler,drawConstants.receiveShadow,
        input.worldPosition, normal, viewDirection, albedo, metallic, roughness);

    const float3 ambientFresnel = FresnelSchlickRoughness(
        max(dot(normal, viewDirection), 0.0f),
        f0,
        roughness);
    const float3 ambientDiffuse =
        kD *
        albedo *
        lighting.ambientColor.rgb *
        lighting.ambientColor.a;
    const float3 ambientSpecular =
        ambientFresnel *
        lighting.environmentColor.rgb *
        lighting.environmentColor.a *
        ambientSpecularStrength;
    const float3 ambient =
        (ambientDiffuse + ambientSpecular);

    float3 color = lighting.lightCounts.w > 0.5 ? ambient + directLight : albedo;
    if(lighting.pbrParams.z >= 0.0f) {
        color *= lighting.pbrParams.w;
        color = color / (color + 1.0f);
        if (lighting.pbrParams.z > 0.5f) color = pow(color, float3(1.0f / 2.2f));
    }
    return float4(color, drawConstants.baseColor.a);
}
