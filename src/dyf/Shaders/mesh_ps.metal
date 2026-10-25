#include <metal_stdlib>
// 이 셰이더가 사용하는 바인딩과 입력 크기다.
#ifndef RENDERER_BINDLESS // 재질 텍스처 배열 방식이 지정되지 않았으면 개별 텍스처 바인딩을 기본으로 사용한다.
#define RENDERER_BINDLESS 0
#endif
#define RENDERER_MAX_DIRECTIONAL_LIGHTS 4
#define RENDERER_MAX_POINT_LIGHTS 16
#define RENDERER_MAX_SPOT_LIGHTS 16
#define RENDERER_MAX_RECT_AREA_LIGHTS 4
#define RENDERER_MAX_DISC_AREA_LIGHTS 4

#define RENDERER_BINDING_BASE_COLOR_TEXTURE 0
#define RENDERER_BINDING_LIGHTING_CONSTANTS 1
#if RENDERER_BINDLESS // bindless 빌드이면 재질 배열·인덱스 표와 그에 맞는 그림자 연결 번호를 사용한다.
#define RENDERER_BINDING_SHADOW_TEXTURE 28
#else
#define RENDERER_BINDING_SHADOW_TEXTURE 2
#endif
#define RENDERER_BINDING_SHADOW_MATRIX 3
#define RENDERER_BINDING_METALLIC_ROUGHNESS_TEXTURE 4
#define RENDERER_BINDING_NORMAL_TEXTURE 5
#define RENDERER_BINDING_OCCLUSION_TEXTURE 6
#define RENDERER_BINDING_EMISSIVE_TEXTURE 7
#define RENDERER_BINDING_MATERIAL_SAMPLER 8
#define RENDERER_BINDING_SHADOW_SAMPLER 9
#define RENDERER_BINDING_INLINE_CONSTANTS 10

#define RENDERER_TEXTURE_FLAG_BASE_COLOR 1
#define RENDERER_TEXTURE_FLAG_METALLIC_ROUGHNESS 2
#define RENDERER_TEXTURE_FLAG_NORMAL 4
#define RENDERER_TEXTURE_FLAG_OCCLUSION 8
#define RENDERER_TEXTURE_FLAG_EMISSIVE 16
#define RENDERER_TEXTURE_FLAG_RECEIVE_SHADOW 32

#ifndef RENDERER_ENABLE_SHADOWS // 그림자 사용 여부를 파이프라인이 명시하지 않았으면 컴파일 오류를 내어 자원 배치 불일치를 막는다.
#error RENDERER_ENABLE_SHADOWS must be defined
#endif

#ifndef RENDERER_FRAGMENT_ENTRY // Metal 픽셀 진입점 이름이 없으면 컴파일을 중단한다.
#error RENDERER_FRAGMENT_ENTRY must be defined
#endif

using namespace metal;
#if !RENDERER_BINDLESS // 개별 바인딩 경로에서만 명명된 재질·화면 환경광 자원을 포함한다. bindless 경로는 재질 배열을 사용한다.
// 개별 바인딩의 환경 상수는 연결 14에 있다. settings.x/y/z는 환경 사용·강도·회전이다.
// 주변 형상에 가려 주변광이 덜 닿는 정도를 나타내는 값(AO)의 사용 여부를 screen.z로 전달한다.
// screen.xy는 화면 역크기, w는 환경 기능 비트다. SH 9개와 probe 정보도 CPU와 같은 메모리 배치를 사용한다.
struct EnvironmentConstants
{
    float4 sh[9];
    float4 settings;
    float4 screen;
    float4 probePosition;
    float4 probeBoxMin;
    float4 probeBoxMax;
};
// 표면에 수직인 방향(법선)의 XY를 환경 회전만큼 돌리고 9개 구면조화 계수로 확산 입사광을 복원한다. 음수 결과는 버리며, 호출측이 albedo·1/pi·환경 강도를 곱한다.
inline float3 EnvironmentIrradiance(float3 n,constant EnvironmentConstants& environment)
{
    const float c=cos(environment.settings.z),s=sin(environment.settings.z);
    n.xy=float2(c*n.x+s*n.y,-s*n.x+c*n.y);
    const float3 value=environment.sh[0].rgb*0.28209479177387814f+
        environment.sh[1].rgb*(0.4886025119029199f*n.y)+environment.sh[2].rgb*(0.4886025119029199f*n.z)+
        environment.sh[3].rgb*(0.4886025119029199f*n.x)+environment.sh[4].rgb*(1.0925484305920792f*n.x*n.y)+
        environment.sh[5].rgb*(1.0925484305920792f*n.y*n.z)+environment.sh[6].rgb*(0.31539156525252005f*(3*n.z*n.z-1))+
        environment.sh[7].rgb*(1.0925484305920792f*n.x*n.z)+environment.sh[8].rgb*(0.5462742152960396f*(n.x*n.x-n.y*n.y));
    return max(value,0.0f);
}
#endif

// 한 draw의 공통 행렬과 재질 입력이다. 정점 단계와 192바이트 배치를 공유하며 draw 상수는 GLSL push constant, HLSL b10, Metal buffer 10으로 전달된다.
// padding0은 알파 cutoff의 float 비트, padding1 하위 2비트는 Opaque/Mask/Blend, 4는 양면, 8은 법선 Y 반전, 16은 unlit이다. padding2는 bindless 재질 표 번호다.
struct DrawConstants
{
    float4x4 viewProjectionMatrix;
    float4x4 modelMatrix;
    uint textureFlags;
    uint padding0;
    uint padding1;
    uint padding2;
    float4 emissiveColor;
    float4 baseColor;
    float4 materialParams;
};

// CPU가 업로드하는 조명 버퍼의 필드 순서와 크기를 유지한다.
// 방향광은 표면에서 광원으로 향하는 방향·조도 강도·색을 가진다. 점/스폿 광원과 달리 거리 감쇠를 적용하지 않는다.
struct RendererDirectionalLight
{
	float4 directionIntensity;
	float4 color;
};

// CPU 공통 조명 버퍼의 점광원 위치·범위·색·강도 배치다. 현재 점/스폿 직접광 순회는 아래 ClusterLight 저장 버퍼를 사용한다.
struct RendererPointLight
{
	float4 positionRange;
	float4 colorIntensity;
};

// 스폿의 위치·거리 범위·방출 방향·내외 원뿔 계수를 공유 배치로 유지한다. 실제 군집 광원 계산에서는 같은 의미의 ClusterLight 필드를 읽는다.
struct RendererSpotLight
{
	float4 positionRange;
	float4 directionOuterCos;
	float4 colorIntensity;
	float4 coneParams;
};

// 사각 발광면 중심·강도, 방출 방향·전체 폭, up 축·전체 높이, RGB다. 이 사각형 경계를 LTC 적분에 넘긴다.
struct RendererRectAreaLight
{
	float4 positionIntensity;
	float4 directionWidth;
	float4 upHeight;
	float4 color;
};

// 원판 발광면의 중심·강도, 방향·반지름, up 축, RGB다. 원 경계를 16각형으로 근사해 LTC로 적분한다.
struct RendererDiscAreaLight
{
	float4 positionIntensity;
	float4 directionRadius;
	float4 up;
	float4 color;
};

// CPU가 올린 카메라·환경·조명·그림자 설정이다. 이전 호환 필드도 포함하므로 일부를 현재 계산이 쓰지 않더라도 구조체 필드 순서는 유지한다.
struct RendererLightingConstants
{
	float4 cameraPosition;
	float4 directionalLightDirection;
	float4 directionalLightColor;
	float4 ambientColor;
	float4 shadowParams;
	float4 pbrParams;
	float4 environmentColor;
	float4 pointLightPositionRange;
	float4 pointLightColorIntensity;
	float4 lightCounts;
	float4 areaLightCounts;
	RendererDirectionalLight directionalLights[RENDERER_MAX_DIRECTIONAL_LIGHTS];
	RendererPointLight pointLights[RENDERER_MAX_POINT_LIGHTS];
	RendererSpotLight spotLights[RENDERER_MAX_SPOT_LIGHTS];
	RendererRectAreaLight rectAreaLights[RENDERER_MAX_RECT_AREA_LIGHTS];
	RendererDiscAreaLight discAreaLights[RENDERER_MAX_DISC_AREA_LIGHTS];
	float4 shadowLight;
};

// 표면을 따라가는 방향(접선)을 정점에서 받는다.
// UV·월드 위치·법선·접선을 픽셀 위치로 보간하고, front_facing으로 받은 면 앞뒤 정보를 양면 재질에 반영한다.
struct RasterData
{
    float4 position [[position]];
    float2 uv [[user(locn0)]];
    float3 worldPosition [[user(locn1)]];
    float3 worldNormal [[user(locn2)]];
    float4 worldTangent [[user(locn3)]];
#if RENDERER_ENABLE_SHADOWS // 그림자 빌드에서만 깊이 자원·시점 자료·가시율 계산을 포함한다. 꺼진 직접광 경로의 가시율은 1이다.
    float4 lightSpacePosition [[user(locn4)]];
#endif
};

constant uint kTextureFlagBaseColor =
    uint(RENDERER_TEXTURE_FLAG_BASE_COLOR);
constant uint kTextureFlagMetallicRoughness =
    uint(RENDERER_TEXTURE_FLAG_METALLIC_ROUGHNESS);
constant uint kTextureFlagNormal =
    uint(RENDERER_TEXTURE_FLAG_NORMAL);
constant uint kTextureFlagOcclusion =
    uint(RENDERER_TEXTURE_FLAG_OCCLUSION);
constant uint kTextureFlagEmissive =
    uint(RENDERER_TEXTURE_FLAG_EMISSIVE);
constant float kPi = 3.14159265359f;

// 거칠기의 제곱을 미세면 분포 폭으로 쓰는 GGX 분포다. 두 벡터 모두에 수직인 벡터를 구하는 연산(외적)을 법선과 반각 벡터에 적용한다.
// 날카로운 반사에서 1-(N·H)^2를 직접 빼면 유효 자릿수를 잃기 쉬우므로 같은 값을 외적 결과의 길이 제곱으로 계산한다.
inline float DistributionGGX(float3 normal, float3 halfway, float roughness)
{
    const float a = roughness * roughness;
    const float a2 = a * a;
    const float ndoth = clamp(dot(normal, halfway), 0.0f, 1.0f);
    const float3 crossNH = cross(normal, halfway);
    const float denominator = dot(crossNH, crossNH) + ndoth * ndoth * a2;
    return a2 / (kPi * denominator * denominator);
}

// 시선이나 광원이 비스듬할 때 미세 표면끼리 가리는 정도를 근사한다. 직접광에서 시선 항과 광원 항을 곱해 사용한다.
inline float GeometrySchlickGGX(float ndotv, float roughness)
{
    const float r = roughness + 1.0f;
    const float k = (r * r) / 8.0f;
    return ndotv / max(ndotv * (1.0f - k) + k, 0.0001f);
}

// 정면 반사율 f0에서 시작해 비스듬히 볼수록 반사가 커지는 Fresnel 효과를 5제곱으로 근사한다.
inline float3 FresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0f - f0) *
        pow(saturate(1.0f - cosTheta), 5.0f);
}

// 주변광과 면광원 확산 가중치용 Fresnel 근사다. 거친 표면에서 가장자리 반사가 과해지지 않도록 roughness로 상한을 조절한다.
inline float3 FresnelSchlickRoughness(
    float cosTheta,
    float3 f0,
    float roughness)
{
    const float3 roughnessF0 = max(float3(1.0f - roughness), f0);
    return f0 + (roughnessF0 - f0) *
        pow(saturate(1.0f - cosTheta), 5.0f);
}

#if RENDERER_ENABLE_SHADOWS // 그림자 빌드에서만 깊이 자원·시점 자료·가시율 계산을 포함한다. 꺼진 직접광 경로의 가시율은 1이다.
// 광원별 시작 시점·개수·강도·광원 크기, 캐스케이드 경계, 아틀라스 영역과 PCSS용 투영 계수를 공유한다. 그림자 좌표는 선택한 시점 행렬로 월드 위치를 다시 투영해 계산한다.
struct ShadowMatrix {
    float4x4 lightViewProjectionMatrix[128];
    float4 atlasRect[128];
    float4 directionalViews[RENDERER_MAX_DIRECTIONAL_LIGHTS];
    float4 pointViews[RENDERER_MAX_POINT_LIGHTS];
    float4 spotViews[RENDERER_MAX_SPOT_LIGHTS];
    float4 directionalSplits[RENDERER_MAX_DIRECTIONAL_LIGHTS];
    float4x4 cameraView;
    float4 filterParams;
    float4 viewProjectionParams[128];
    float4 softShadowParams;
};

#define DY_SHADOW_STRUCT ShadowMatrix
#define DY_SHADOW_TEXTURE depth2d<float>
#define DY_SHADOW_PROJECTION(index) shadowData.viewProjectionParams[index]
#define DY_SHADOW_MATRIX(index) shadowData.lightViewProjectionMatrix[index]
#define DY_SHADOW_TILE(index) shadowData.atlasRect[index]
#define DY_SHADOW_DIMENSIONS int2(shadowImage.get_width(),shadowImage.get_height())
#define DY_SHADOW_READ(pixel) shadowImage.read(uint2(pixel))
#define DY_SHADOW_FILTER shadowData.filterParams
#define DY_SHADOW_SOFT shadowData.softShadowParams
// 현재 자원 읽기 함수를 DY_SHADOW 매크로로 연결한 뒤 공통 PCSS 구현을 포함한다. 같은 필터 알고리즘을 언어별 타입과 바인딩에 맞춰 쓴다.
#include "shadow_filter.metal"

// 한 그림자 시점의 가시율을 구한다. 0~1 깊이와 위아래를 뒤집은 UV를 아틀라스 영역에 넣고, 같은 표면이 자기 그림자로 판정되는 오차를 줄이도록 표면 기울기 기반 bias를 수신 깊이에서 뺀다.
// 광원 크기가 있으면 PCSS로 반그림자를, 없으면 고정 텍셀 반경 PCF로 가시 표본 비율을 구한다. strength는 최종 어두워지는 양만 조절한다.
float SampleShadowView(int index,float3 worldPosition,float3 normal,float3 lightDir,float strength,float sourceRadius,int pointBase,
    constant RendererLightingConstants& lighting,constant ShadowMatrix& shadowMatrix,depth2d<float> shadowMap,sampler shadowSampler)
{
    float4 clip=shadowMatrix.lightViewProjectionMatrix[index]*float4(worldPosition,1.0);
    if(clip.w<=0.0)return 1.0; // 선택한 광원 시점 뒤쪽이면 해당 시점 그림자를 적용하지 않는다.
    float3 ndc=clip.xyz/clip.w;
    float2 uv=ndc.xy*.5+.5;
    uv.y=1.0-uv.y;
    if(uv.x<0.0||uv.y<0.0||uv.x>1.0||uv.y>1.0||ndc.z<0.0||ndc.z>1.0)return 1.0; // 그림자 시점의 UV 또는 0~1 깊이 범위 밖이면 완전 가시로 둔다.
    float4 tile=shadowMatrix.atlasRect[index];
    float2 texel=1.0/float2(shadowMap.get_width(),shadowMap.get_height());
    float2 minUv=tile.xy+texel*.5;
    float2 maxUv=tile.xy+tile.zw-texel*.5;
    float2 localUv=uv;
    uv=tile.xy+uv*tile.zw;
    float angle=1.0-max(dot(normal,lightDir),0.0);
    float bias=max(lighting.shadowParams.x,lighting.shadowParams.y*angle)+lighting.shadowParams.z*angle;
    float receiver=ndc.z-bias;
    if(sourceRadius>0 && shadowMatrix.softShadowParams.x>0) // 광원 크기와 허용 소프트 반경이 모두 양수일 때만 PCSS를 사용한다.
        return 1-clamp(strength,0.0,1.0)*(1-DyShadowPcss(index,pointBase,localUv,receiver,clip.w,lightDir,sourceRadius,shadowMatrix,shadowMap));
    float radius=min(lighting.shadowParams.w,shadowMatrix.filterParams.z);
    int extent=int(ceil(radius));
    float visible=0.0,samples=0.0;
    for(int y=-extent;y<=extent;++y)for(int x=-extent;x<=extent;++x)
    {
        float2 sampleUv=clamp(uv+float2(x,y)*texel,minUv,maxUv);
        visible+=receiver<=shadowMap.sample(shadowSampler,sampleUv)?1.0:0.0;
        samples+=1.0;
    }
    return 1.0-clamp(strength,0.0,1.0)*(1.0-visible/max(samples,1.0));
}

// 방향광은 카메라 깊이의 캐스케이드, 점광원은 광원→표면 방향의 큐브면, 스폿은 해당 시점을 고른다. 방향광 경계 마지막 10%에서는 다음 캐스케이드와 섞는다.
float SampleShadow(int type,int lightIndex,float3 worldPosition,float3 normal,float3 lightDir,
    constant RendererLightingConstants& lighting,constant ShadowMatrix& shadowMatrix,depth2d<float> shadowMap,sampler shadowSampler,uint drawFlags)
{
    if((drawFlags & RENDERER_TEXTURE_FLAG_RECEIVE_SHADOW)==0u)return 1.0; // 객체의 그림자 받기 비트가 꺼져 있으면 가시율 1로 조명을 그대로 받는다.
    float4 views=type==1?shadowMatrix.directionalViews[lightIndex]:(type==2?shadowMatrix.pointViews[lightIndex]:shadowMatrix.spotViews[lightIndex]);
    int count=int(views.y+.5);
    if(count==0)return 1.0; // 광원에 배정된 그림자 시점이 없으면 가시율 1을 반환한다.
    int local=0;
    float depth=0.0;
    if(type==1) // 방향광이면 카메라 앞쪽 거리로 캐스케이드를 선택한다.
    {
        depth=-(shadowMatrix.cameraView*float4(worldPosition,1.0)).z;
        float finalSplit=shadowMatrix.directionalSplits[lightIndex][count-1];
        if(finalSplit>0 && depth>finalSplit)return 1.0; // 마지막 유효 캐스케이드 거리보다 멀면 그림자를 적용하지 않는다.
        while(local<count-1 && depth>shadowMatrix.directionalSplits[lightIndex][local])++local;
    }
    if(type==2) // 점광원이면 광원에서 표면으로 향하는 방향의 주요 축으로 큐브면을 고른다.
    {
        float3 direction=-lightDir, magnitude=abs(direction);
        if(magnitude.x>=magnitude.y && magnitude.x>=magnitude.z)local=direction.x>=0?0:1; // X축이 주요 방향이면 부호에 따라 +X/-X 시점을 고른다.
        else if(magnitude.y>=magnitude.z)local=direction.y>=0?2:3; // X축이 아니고 Y축이 Z축 이상이면 +Y/-Y 시점을 고른다.
        else local=direction.z>=0?4:5;
    }
    int index=int(views.x+.5)+local;
    float visibility=SampleShadowView(index,worldPosition,normal,lightDir,views.z,views.w,type==2?int(views.x+.5):-1,lighting,shadowMatrix,shadowMap,shadowSampler);
    if(type==1 && local<count-1) // 방향광의 마지막 캐스케이드가 아니면 현재 거리 구간의 끝 10%에서 다음 시점과 섞을 비율을 계산한다.
    {
        float previous=local==0?shadowMatrix.filterParams.w:shadowMatrix.directionalSplits[lightIndex][local-1];
        float split=shadowMatrix.directionalSplits[lightIndex][local];
        float blend=smoothstep(mix(previous,split,.9f),split,depth);
        if(blend>0.0)visibility=mix(visibility, // 캐스케이드 경계의 혼합 구간에서만 다음 시점 그림자도 읽는다.
            SampleShadowView(index+1,worldPosition,normal,lightDir,views.z,views.w,type==2?int(views.x+.5):-1,lighting,shadowMatrix,shadowMap,shadowSampler),blend);
    }
    return visibility;
}

#endif

// 여기서 구한 거리 감쇠는 아래 국소 광원 루프에서 광원 RGB·강도에 곱해 EvaluateDirectLight의 입사 조도로 전달한다.
// 거리 제곱 역수에 거리 범위의 끝에서 0으로 부드럽게 닫히는 창 함수를 곱한다. 광원과 아주 가까운 거리의 분모는 작은 양수로 제한한다.
inline float PunctualAttenuation(float distanceToLight, float range) {
    if (range <= 0.0 || distanceToLight >= range) { // 도달 범위가 없거나 범위 끝을 넘은 광원은 기여가 0이다.
        return 0.0;
    }
    float distanceSquared = max(distanceToLight * distanceToLight, 0.0001);
    float ratio = distanceToLight / range;
    float ratioSquared = ratio * ratio;
    float window = max(1.0 - ratioSquared * ratioSquared, 0.0);
    return (window * window) / distanceSquared;
}

// 표면 방향과 스폿 방출축의 cos 값을 내부·외부 원뿔 사이의 0~1로 옮긴 뒤 부드러운 3차 곡선으로 감쇠한다.
inline float SpotConeAttenuation(float cosTheta, float innerCos, float outerCos) {
    float denominator = max(innerCos - outerCos, 0.0001);
    float t = clamp((cosTheta - outerCos) / denominator, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// 한 광원에 대한 GGX 반사와 에너지 가중 Lambert 확산을 더한다. 입사 조도·ndotl·그림자 가시율을 곱하며, ndotl은 max(dot(normal, lightDir), 0)이다. 금속도가 높을수록 확산 비중은 줄어든다.
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
    if (ndotl <= 0.0 || ndotv <= 0.0 || visibility <= 0.0) { // 광원 또는 시선이 법선의 반대쪽이나 표면과 나란한 방향에 있거나, 가시율이 0 이하면 이 광원의 기여 RGB를 0으로 반환한다.
        return float3(0.0,0.0,0.0);
    }
    float3 halfwayVector = viewDir + lightDir;
    float halfwayLengthSquared = dot(halfwayVector, halfwayVector);
    if (halfwayLengthSquared <= 0.000001) { // 시선과 광원 방향이 상쇄되어 반각 벡터를 정규화할 수 없으면 0을 반환한다.
        return float3(0.0,0.0,0.0);
    }
    float3 halfway = halfwayVector * rsqrt(halfwayLengthSquared);
    float3 f0 = mix(float3(0.04,0.04,0.04), albedo, metallic);
    float ndf = DistributionGGX(normal, halfway, roughness);
    float geometry = GeometrySchlickGGX(ndotv, roughness) * GeometrySchlickGGX(ndotl, roughness);
    float3 fresnel = FresnelSchlick(max(dot(halfway, viewDir), 0.0), f0);
    float3 specular = (ndf * geometry * fresnel) / max(4.0 * ndotv * ndotl, 0.0001);
    float3 diffuseWeight = (float3(1.0,1.0,1.0) - fresnel) * (1.0 - metallic);
    return (diffuseWeight * albedo / kPi + specular) * incidentIlluminance * ndotl * visibility;
}

// LTC 적분과 표 배치는 Heitz 외 SIGGRAPH 2016 및 Assets/ltc/LICENSE를 따른다.
// 정규화된 두 경계 방향 사이의 구면 모서리 적분을 계산한다. 모서리들을 순서대로 더해 면광원이 차지하는 cosine 가중 영역을 얻는다.
inline float LtcEdgeIntegral(float3 a,float3 b)
{
    float3 edge=cross(a,b);
    float sine=length(edge),cosine=clamp(dot(a,b),-1.0,1.0);
    return edge.z*(atan2(sine,cosine)/max(sine,1e-12))*0.15915494309189535;
}
// 광원 꼭짓점의 상대 위치를 표면 접선 기준축으로 옮긴 뒤 LTC 역행렬을 적용한다. 표 조회 결과의 4성분만으로 필요한 변환을 구성한다.
inline float3 LtcTransform(float3 offset,float3 tangent,float3 bitangent,float3 normal,float4 inverseMatrix)
{
    float3 local=float3(dot(offset,tangent),dot(offset,bitangent),dot(offset,normal));
    return float3(inverseMatrix.x*local.x+inverseMatrix.z*local.z,local.y,
        inverseMatrix.y*local.x+inverseMatrix.w*local.z);
}
// 면광원 경계를 LTC 공간으로 옮기고 z>=0인 표면 위쪽 반구에 자른다. 잘린 점을 정규화하며 바로 모서리 적분에 더하므로 별도 잘린 다각형 배열이 필요 없다.
inline float LtcPolygonIntegral(float3 normal,float3 viewDir,float3 position,
    float4 inverseMatrix,thread const float3* points,int count)
{
    float3 tangent=viewDir-normal*dot(normal,viewDir);
    if(dot(tangent,tangent)<1e-8) // 시선의 접선 성분이 너무 작으면 법선과 평행하지 않은 축으로 접선을 만든다.
        tangent=cross(abs(normal.z)<.999?float3(0,0,1):float3(0,1,0),normal);
    tangent=normalize(tangent);
    float3 bitangent=cross(normal,tangent);
    float3 previous=LtcTransform(points[count-1]-position,tangent,bitangent,normal,inverseMatrix);
    float3 first=float3(0),last=float3(0);bool haveVertex=false;float sum=0;
    for(int edge=0;edge<count;++edge)
    {
        float3 current=LtcTransform(points[edge]-position,tangent,bitangent,normal,inverseMatrix);
        bool previousInside=previous.z>=0,currentInside=current.z>=0;
        for(int candidate=0;candidate<2;++candidate)
        {
            float3 point;
            if(candidate==0) // 먼저 모서리와 z=0 경계의 교점을 처리하고 다음 후보에서 원래 끝점을 처리한다.
            {
                if(previousInside==currentInside)continue; // 두 끝점이 같은 쪽이면 z=0을 가로지르지 않아 교점 후보가 없다.
                point=mix(previous,current,clamp(previous.z/(previous.z-current.z),0.0,1.0));
                point.z=0;
            }
            else
            {
                if(!currentInside)continue; // 원래 끝점이 표면 아래 반구에 있으면 잘린 다각형에서 제외한다.
                point=current;
            }
            float squared=dot(point,point);
            if(squared<=1e-20)continue; // 원점에 너무 가까워 방향을 정규화할 수 없는 경계점은 건너뛴다.
            point*=rsqrt(squared);
            if(haveVertex)sum+=LtcEdgeIntegral(last,point);else {first=point;haveVertex=true;} // 첫 점은 시작점으로 저장하고 이후 점은 직전 점과의 모서리 적분을 더한다.
            last=point;
        }
        previous=current;
    }
    if(haveVertex)sum+=LtcEdgeIntegral(last,first); // 점이 하나라도 있으면 마지막 점에서 첫 점으로 경계를 닫는다.
    return clamp(abs(sum),0.0,1.0);
}
// 거칠기와 N·V로 64×64 LTC 표의 변환·진폭을 읽는다. 반사 적분과 단위행렬의 확산 적분을 별도로 구해 금속도·Fresnel 가중치와 발광면 radiance를 곱한다.
inline float3 EvaluateLtcPolygon(float3 normal,float3 viewDir,float3 position,thread const float3* points,int count,
    float3 albedo,float metallic,float roughness,float3 radiance,texture2d<float> ltcMatrixTexture,texture2d<float> ltcAmplitudeTexture,sampler materialSampler)
{
    float ndotv=clamp(dot(normal,viewDir),0.0,1.0);
    if(ndotv<=0)return float3(0); // 시선이 표면 뒤쪽이면 이 면광원 BRDF 기여는 0이다.
    float2 coordinate=(float2(clamp(roughness,0.0,1.0),sqrt(1-ndotv))*63.0+.5)/64.0;
    float4 inverseMatrix=ltcMatrixTexture.sample(materialSampler,coordinate,level(0));
    float4 amplitude=ltcAmplitudeTexture.sample(materialSampler,coordinate,level(0));
    float specular=LtcPolygonIntegral(normal,viewDir,position,inverseMatrix,points,count);
    float diffuse=LtcPolygonIntegral(normal,viewDir,position,float4(1,0,0,1),points,count);
    float3 f0=mix(float3(.04),albedo,metallic);
    float3 specularWeight=f0*max(amplitude.x,0.0)+(float3(1)-f0)*max(amplitude.y,0.0);
    float3 diffuseWeight=(float3(1)-FresnelSchlickRoughness(ndotv,f0,roughness))*(1-metallic);
    // 정규화된 cosine 적분이므로 diffuse에 1/pi나 거리 감쇠를 다시 곱하지 않는다.
    return radiance*(specularWeight*specular+diffuseWeight*albedo*diffuse);
}
// 방출 방향과 up으로 사각형 축을 만들고 네 모서리를 적분한다. 폭·높이가 없거나 발광면 뒤에 있는 표면은 빛을 받지 않는다.
inline float3 EvaluateRectAreaLight(RendererRectAreaLight light,float3 normal,float3 viewDir,
    float3 albedo,float metallic,float roughness,float3 worldPosition,texture2d<float> ltcMatrixTexture,texture2d<float> ltcAmplitudeTexture,sampler materialSampler)
{
    float3 direction=normalize(light.directionWidth.xyz);
    if(light.directionWidth.w<=0 || light.upHeight.w<=0 || dot(direction,worldPosition-light.positionIntensity.xyz)<=0) // 면적이 없거나 사각 광원의 뒷면에 있는 수신점은 조명하지 않는다.
        return float3(0);
    float3 right=normalize(cross(direction,light.upHeight.xyz));
    float3 up=normalize(cross(right,direction));
    float3 x=right*(light.directionWidth.w*.5),y=up*(light.upHeight.w*.5),center=light.positionIntensity.xyz;
    float3 points[16];
    points[0]=center-x-y;points[1]=center+x-y;points[2]=center+x+y;points[3]=center-x+y;
    return EvaluateLtcPolygon(normal,viewDir,worldPosition,points,4,albedo,metallic,roughness,
        light.color.rgb*max(light.positionIntensity.w,0.0),ltcMatrixTexture,ltcAmplitudeTexture,materialSampler);
}
// 원판 경계를 같은 각도 간격의 16각형으로 근사해 LTC로 면 전체를 적분하며, 점광원 16개의 합이나 거리 감쇠 표본 방식으로 계산하지 않는다.
inline float3 EvaluateDiscAreaLight(RendererDiscAreaLight light,float3 normal,float3 viewDir,
    float3 albedo,float metallic,float roughness,float3 worldPosition,texture2d<float> ltcMatrixTexture,texture2d<float> ltcAmplitudeTexture,sampler materialSampler)
{
    float3 direction=normalize(light.directionRadius.xyz);
    if(light.directionRadius.w<=0 || dot(direction,worldPosition-light.positionIntensity.xyz)<=0)return float3(0); // 반지름이 없거나 원판 광원의 뒷면에 있는 수신점은 조명하지 않는다.
    float3 right=normalize(cross(direction,light.up.xyz));
    float3 up=normalize(cross(right,direction));
    float3 points[16];
    for(int corner=0;corner<16;++corner)
    {
        float angle=float(corner)*0.39269908169872415;
        points[corner]=light.positionIntensity.xyz+(right*cos(angle)+up*sin(angle))*light.directionRadius.w;
    }
    return EvaluateLtcPolygon(normal,viewDir,worldPosition,points,16,albedo,metallic,roughness,
        light.color.rgb*max(light.positionIntensity.w,0.0),ltcMatrixTexture,ltcAmplitudeTexture,materialSampler);
}

// 국소 광원의 위치/거리, RGB/강도, 스폿 방향/외곽 cos, 내부 cos·광원 종류·그림자 번호를 한 항목으로 묶는다.
// 연결 19의 광원 배열, 20의 군집 목록, 21의 군집 투영 상수를 함께 사용한다. GLSL은 set 0, HLSL은 t19/t20/b21, Metal은 buffer 19/20/21이다.
struct ClusterLight {float4 positionRange;float4 colorIntensity;float4 directionOuterCos;float4 coneShadow;};
struct ClusterParameters {float4x4 view;float4x4 projection;float4x4 inverseProjection;float4 depth;uint4 dimensions;uint4 counts;};
// 월드 위치를 군집 카메라로 투영하여 화면 XY와 깊이 슬라이스를 구한다. 원근 카메라는 로그 깊이, 직교 카메라는 선형 깊이를 쓴다.
// 반환값은 개수 1칸과 광원 인덱스 목록이 이어진 군집 구간의 시작이다. 0xffffffff는 목록을 사용할 수 없다는 표시이며 전체 국소 광원 순회로 대체한다.
inline uint ClusterBase(float3 position,constant ClusterParameters& clusters)
{
    if(clusters.counts.y==0u)return 0xffffffffu; // 군집 목록 사용이 꺼져 있으면 전체 국소 광원 순회 표식을 반환한다.
    float4 viewPosition=clusters.view*float4(position,1),clip=clusters.projection*viewPosition;
    float distance=-viewPosition.z;
    if(distance<clusters.depth.x || distance>clusters.depth.y || clip.w<=0)return 0xffffffffu; // 군집 깊이 범위 밖이거나 투영 뒤쪽이면 유효한 군집을 고를 수 없다.
    float2 coordinate=clip.xy/clip.w*.5f+.5f;
    if(any(coordinate<0) || any(coordinate>=1))return 0xffffffffu; // 군집 화면 범위 [0,1) 밖이면 전체 국소 광원 경로로 돌린다.
    float slice=clusters.counts.z!=0u?log(distance/clusters.depth.x)/clusters.depth.z:
        (distance-clusters.depth.x)/(clusters.depth.y-clusters.depth.x);
    uint3 index=min(uint3(float3(coordinate*float2(clusters.dimensions.xy),slice*float(clusters.dimensions.z))),clusters.dimensions.xyz-uint3(1));
    return (index.x+clusters.dimensions.x*(index.y+clusters.dimensions.y*index.z))*(clusters.dimensions.w+1u);
}

// 방향광 → 군집의 점/스폿 광원 → 사각·원판 면광원 순서로 직접광을 누적한다. 군집을 쓸 수 없거나 목록이 overflow 표시면 전체 국소 광원을 검사한다.
inline float3 EvaluateLights(constant RendererLightingConstants& lighting,
    const device ClusterLight* clusterLights,const device uint* clusterList,constant ClusterParameters& clusters,
    texture2d<float> ltcMatrixTexture,texture2d<float> ltcAmplitudeTexture,sampler materialSampler,
#if RENDERER_ENABLE_SHADOWS // 그림자 빌드에서만 깊이 자원·시점 자료·가시율 계산을 포함한다. 꺼진 직접광 경로의 가시율은 1이다.
    constant ShadowMatrix& shadowMatrix, depth2d<float> shadowMap, sampler shadowSampler, uint drawFlags,
#endif
    float3 worldPosition, float3 normal, float3 viewDir,
    float3 albedo, float metallic, float roughness) {
    float3 directLight = float3(0.0,0.0,0.0);
    int directionalCount = clamp(int(lighting.lightCounts.x + 0.5), 0, RENDERER_MAX_DIRECTIONAL_LIGHTS);
    for (int lightIndex = 0; lightIndex < directionalCount; ++lightIndex) {
        RendererDirectionalLight light = lighting.directionalLights[lightIndex];
        float3 lightDir = normalize(light.directionIntensity.xyz);
#if RENDERER_ENABLE_SHADOWS // 그림자 빌드에서만 깊이 자원·시점 자료·가시율 계산을 포함한다. 꺼진 직접광 경로의 가시율은 1이다.
        float visibility = SampleShadow(1,lightIndex,worldPosition,normal,lightDir,lighting,shadowMatrix,shadowMap,shadowSampler,drawFlags);
#else
        float visibility = 1.0;
#endif
        directLight += EvaluateDirectLight(normal, viewDir, lightDir,
            light.color.rgb * max(light.directionIntensity.w, 0.0), albedo, metallic, roughness, visibility);
    }
    uint base=ClusterBase(worldPosition,clusters);
    uint listed=0xffffffffu;
    if(base!=0xffffffffu)listed=clusterList[base]; // 유효한 군집 주소일 때만 목록 개수를 읽는다. 무효 주소를 버퍼 인덱스로 쓰지 않는다.
    // 목록의 첫 칸이 0xffffffff이면 누락 없이 전체 clusterLights를 순회한다. 정상 목록은 해당 군집에 들어간 인덱스만 순회한다.
    uint localCount=listed==0xffffffffu?clusters.counts.x:listed;
    for(uint item=0u;item<localCount;++item)
    {
        uint lightIndex=listed==0xffffffffu?item:clusterList[base+1u+item];
        ClusterLight light=clusterLights[lightIndex];
        float3 toLight=light.positionRange.xyz-worldPosition;
        float distanceToLight=length(toLight);
        if(distanceToLight<=0.0001 || distanceToLight>=light.positionRange.w)continue; // 광원과 거의 같은 위치이거나 거리 범위 밖인 국소 광원은 건너뛴다.
        float3 lightDir=toLight/distanceToLight;
        float attenuation=PunctualAttenuation(distanceToLight,light.positionRange.w);
        int type=int(light.coneShadow.y+0.5),shadowIndex=int(light.coneShadow.z);
        if(type==3)attenuation*=SpotConeAttenuation(dot(-lightDir,light.directionOuterCos.xyz), // 스폿 광원은 거리 감쇠에 원뿔 각도 감쇠를 추가한다.
            light.coneShadow.x,light.directionOuterCos.w);
#if RENDERER_ENABLE_SHADOWS // 그림자 빌드에서만 깊이 자원·시점 자료·가시율 계산을 포함한다. 꺼진 직접광 경로의 가시율은 1이다.
        float visibility=shadowIndex>=0?SampleShadow(type,shadowIndex,worldPosition,normal,lightDir,lighting,shadowMatrix,shadowMap,shadowSampler,drawFlags):1.0;
#else
        float visibility=1.0;
#endif
        float3 incidentIlluminance=light.colorIntensity.rgb*max(light.colorIntensity.w,0.0)*attenuation;
        directLight+=EvaluateDirectLight(normal,viewDir,lightDir,incidentIlluminance,albedo,metallic,roughness,visibility);
    }
    int rectAreaCount = clamp(int(lighting.areaLightCounts.x + 0.5), 0, RENDERER_MAX_RECT_AREA_LIGHTS);
    for (int lightIndex = 0; lightIndex < rectAreaCount; ++lightIndex) {
        directLight += EvaluateRectAreaLight(lighting.rectAreaLights[lightIndex], normal, viewDir, albedo, metallic, roughness, worldPosition,
            ltcMatrixTexture,ltcAmplitudeTexture,materialSampler);
    }
    int discAreaCount = clamp(int(lighting.areaLightCounts.y + 0.5), 0, RENDERER_MAX_DISC_AREA_LIGHTS);
    for (int lightIndex = 0; lightIndex < discAreaCount; ++lightIndex) {
        directLight += EvaluateDiscAreaLight(lighting.discAreaLights[lightIndex], normal, viewDir, albedo, metallic, roughness, worldPosition,
            ltcMatrixTexture,ltcAmplitudeTexture,materialSampler);
    }

    return directLight;
}

// 보간 법선을 정규화하고 접선을 법선에 직교화해 TBN 기준축을 만든다. 법선 맵을 -1~1로 바꾸고 XY 강도·선택적 Y 반전을 적용해 월드 법선으로 옮긴다.
inline float3 ResolveNormal(
    RasterData input,
    constant DrawConstants& drawConstants,
    texture2d<float> normalTexture,
    sampler materialSampler)
{
    float3 normal = normalize(input.worldNormal);
    const uint textureFlags = drawConstants.textureFlags;
    if ((textureFlags & kTextureFlagNormal) == 0u || // 법선 이미지가 없거나 법선 강도가 사실상 0이면 보간된 월드 법선을 그대로 사용한다.
        abs(drawConstants.materialParams.z) <= 0.0001f)
    {
        return normal;
    }

    const float3 tangent = normalize(
        input.worldTangent.xyz -
        normal * dot(normal, input.worldTangent.xyz));
    const float3 bitangent =
        normalize(cross(normal, tangent)) * input.worldTangent.w;
    // GLSL·Metal은 T/B/N을 열로 놓고 행렬×법선을 계산한다. HLSL은 생성자의 행으로 놓은 뒤 법선×행렬을 계산하여 같은 월드 방향을 얻는다.
    const float3x3 tangentBasis = float3x3(tangent, bitangent, normal);
    float3 tangentNormal =
        normalTexture.sample(materialSampler, input.uv).xyz * 2.0f - 1.0f;
    tangentNormal.xy *= drawConstants.materialParams.z;
    if((drawConstants.padding1 & 8u)!=0u) tangentNormal.y=-tangentNormal.y; // 법선 맵의 Y 방향이 반대인 재질이면 접선 공간 Y 성분만 뒤집는다.
    return normalize(tangentBasis * tangentNormal);
}

// 기본색·알파를 먼저 읽고 LOD 표시/unlit를 분리한다. 일반 경로는 재질과 법선 → 직접광·주변광·발광 → 출력 변환 순서로 RGBA를 만든다.
inline float4 RunFragmentShader(
    RasterData input,
    bool frontFace,
    constant DrawConstants& drawConstants,
    constant RendererLightingConstants& lighting,
    const device ClusterLight* clusterLights,const device uint* clusterList,constant ClusterParameters& clusters,
    texture2d<float> ltcMatrixTexture,texture2d<float> ltcAmplitudeTexture,
    texture2d<float> baseColorTexture,
#if RENDERER_ENABLE_SHADOWS // 그림자 빌드에서만 깊이 자원·시점 자료·가시율 계산을 포함한다. 꺼진 직접광 경로의 가시율은 1이다.
    depth2d<float> shadowMap,
    constant ShadowMatrix& shadowMatrix,
#endif
    texture2d<float> metallicRoughnessTexture,
    texture2d<float> normalTexture,
    texture2d<float> occlusionTexture,
    texture2d<float> emissiveTexture,
#if RENDERER_ENABLE_SHADOWS // 그림자 빌드에서만 깊이 자원·시점 자료·가시율 계산을 포함한다. 꺼진 직접광 경로의 가시율은 1이다.
    sampler shadowSampler,
#endif
    sampler materialSampler
#if !RENDERER_BINDLESS // 개별 바인딩 경로에서만 명명된 재질·화면 환경광 자원을 포함한다. bindless 경로는 재질 배열을 사용한다.
    ,texture2d<float> screenAOTexture,texture2d<float> environmentSpecularTexture,texture2d<float> environmentDiffuseTexture,
    constant EnvironmentConstants& environment,sampler screenSampler
#endif
    )
{
    const uint textureFlags = drawConstants.textureFlags;
    float4 base = drawConstants.baseColor;
    if ((textureFlags & kTextureFlagBaseColor) != 0u) // 기본색 이미지가 있으면 재질 기본색 RGBA에 이미지 RGBA를 곱한다.
    {
        base *= baseColorTexture.sample(materialSampler, input.uv);
    }
    // 알파 모드 0은 불투명, 1은 마스크 잘라내기, 2는 블렌드다. 일반 경로에서 마스크 discard는 법선 미분 계산 뒤로 미뤄 픽셀 이웃 미분을 유지한다.
    const uint alphaMode=drawConstants.padding1 & 3u;
    if((textureFlags&64u)!=0u) // LOD 표시용 비트면 텍스처 알파의 마스크만 유지하고 조명·텍스처 RGB를 쓰지 않은 표시색을 낸다.
    {
        if(alphaMode==1u && base.a<as_type<float>(drawConstants.padding0)) discard_fragment(); // Mask 모드에서 cutoff보다 작은 픽셀을 버린다. 그 외 알파 모드는 이 잘라내기를 하지 않는다.
        float3 color=drawConstants.baseColor.rgb;
        if(lighting.pbrParams.z>.5f) color=pow(color,float3(1.0f/2.2f)); // 이 출력 경로가 감마 적용을 요구할 때만 RGB에 1/2.2 거듭제곱을 적용한다.
        return float4(color,alphaMode==2u ? base.a : 1.0f);
    }
    if((drawConstants.padding1&16u)!=0u) // unlit 재질이면 기본색만 출력 변환하고 일반 조명 계산을 건너뛴다.
    {
        if(alphaMode==1u && base.a<as_type<float>(drawConstants.padding0)) discard_fragment(); // Mask 모드에서 cutoff보다 작은 픽셀을 버린다. 그 외 알파 모드는 이 잘라내기를 하지 않는다.
        float3 color=base.rgb;
        if(lighting.pbrParams.z>=0.0f) // 직접 출력 경로만 노출과 톤 매핑을 적용한다. 음수는 선형 HDR로 남겨 후단 출력 패스에 맡긴다.
        {
            color=min(color*lighting.pbrParams.w,float3(16777216.0f));
            color=color/(color+1.0f);
            if(lighting.pbrParams.z>.5f)color=pow(color,float3(1.0f/2.2f)); // 이 출력 경로가 감마 적용을 요구할 때만 RGB에 1/2.2 거듭제곱을 적용한다.
        }
        color=float3(isnan(color.r)?0:clamp(color.r,0.0f,65504.0f),isnan(color.g)?0:clamp(color.g,0.0f,65504.0f),
            isnan(color.b)?0:clamp(color.b,0.0f,65504.0f));
        return float4(color,alphaMode==2u?base.a:1.0f);
    }
    const float3 albedo=base.rgb;

    const float minRoughness =
        clamp(lighting.pbrParams.x, 0.01f, 1.0f);
    const float ambientSpecularStrength =
        max(lighting.pbrParams.y, 0.0f);
    float metallic =
        clamp(drawConstants.materialParams.x, 0.0f, 1.0f);
    float roughness =
        clamp(drawConstants.materialParams.y, minRoughness, 1.0f);
    float occlusion = 1.0f;

    if ((textureFlags & kTextureFlagMetallicRoughness) != 0u) // 이미지 G로 거칠기, B로 금속도를 곱하고 유효 범위로 제한한다.
    {
        const float4 metallicRoughness =
            metallicRoughnessTexture.sample(materialSampler, input.uv);
        roughness = clamp(
            roughness * metallicRoughness.g,
            minRoughness,
            1.0f);
        metallic = clamp(
            metallic * metallicRoughness.b,
            0.0f,
            1.0f);
    }
    if ((textureFlags & kTextureFlagOcclusion) != 0u) // 이미지 R의 AO를 재질 강도와 섞어 주변광 차폐량을 만든다.
    {
        occlusion = mix(1.0f, occlusionTexture.sample(materialSampler, input.uv).r,
            clamp(drawConstants.materialParams.w, 0.0f, 1.0f));
    }

    float3 normal =
        ResolveNormal(input, drawConstants, normalTexture, materialSampler);
    // Derivatives precede discard and the two-sided normal orientation branch.
    const float3 normalDx=dfdx(normal),normalDy=dfdy(normal);
    // 화면 이웃의 법선 변화량으로 좁은 반사의 흔들림을 줄인다. roughness^4에 제한된 분산을 더한 뒤 네제곱근으로 새 roughness를 만든다.
    const float normalVariance=0.25f*(dot(normalDx,normalDx)+dot(normalDy,normalDy));
    const float filteredAlphaSquared=min(1.0f,roughness*roughness*roughness*roughness+min(2.0f*normalVariance,0.18f));
    roughness=sqrt(sqrt(filteredAlphaSquared));
    if(alphaMode==1u && base.a<as_type<float>(drawConstants.padding0)) discard_fragment(); // Mask 모드에서 cutoff보다 작은 픽셀을 버린다. 그 외 알파 모드는 이 잘라내기를 하지 않는다.
    if((drawConstants.padding1 & 4u)!=0u && !frontFace) normal=-normal; // 양면 재질의 뒷면이면 법선을 반대로 돌려 뒤쪽 표면 방향으로 조명한다.
    const float3 viewDirection =
        normalize(lighting.cameraPosition.xyz - input.worldPosition);

    const float3 f0 =
        mix(float3(0.04f), albedo, metallic);
    const float3 directLight = EvaluateLights(lighting,
        clusterLights,clusterList,clusters,
        ltcMatrixTexture,ltcAmplitudeTexture,materialSampler,
#if RENDERER_ENABLE_SHADOWS // 그림자 빌드에서만 깊이 자원·시점 자료·가시율 계산을 포함한다. 꺼진 직접광 경로의 가시율은 1이다.
        shadowMatrix,shadowMap,shadowSampler,drawConstants.textureFlags,
#endif
        input.worldPosition, normal, viewDirection, albedo, metallic, roughness);

    const float3 ambientFresnel = FresnelSchlickRoughness(
        max(dot(normal, viewDirection), 0.0f),
        f0,
        roughness);
    const float3 kD = (float3(1.0f) - ambientFresnel) * (1.0f - metallic);
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
    float3 ambient =
        (ambientDiffuse + ambientSpecular) * occlusion;
#if !RENDERER_BINDLESS // 개별 바인딩 경로에서만 명명된 재질·화면 환경광 자원을 포함한다. bindless 경로는 재질 배열을 사용한다.
    // 장면 픽셀 좌표에 화면 역크기를 곱해 화면 AO·환경광 결과를 읽는다. 이 자원들은 개별 텍스처 경로에만 연결된다.
    const float2 screenUV=input.position.xy*environment.screen.xy;
    const float screenAO=environment.screen.z>0.5 ? screenAOTexture.sample(screenSampler,screenUV,level(0)).r : 1.0f;
    ambient*=screenAO;
    if(environment.settings.x>0.5) // 환경광 패스가 활성화되면 기존 단색 주변광을 0으로 지우고, 알파 모드에 따라 SH 확산광 또는 화면 환경광 텍스처 결과로 다시 계산한다.
    {
        ambient=0;
        if(alphaMode==2u) // 투명 표면은 불투명 표면용 화면 환경광 대신 현재 법선의 SH 확산 경로로 간다.
        {
            if((uint(environment.screen.w+0.5f)&1u)!=0u) // 환경 diffuse 비트가 켜져 있을 때만 투명 표면의 SH 확산광을 더한다.
                ambient=kD*albedo*EnvironmentIrradiance(normal,environment)*(0.3183098861837907f*environment.settings.y*occlusion*screenAO);
        }
        else
        {
            // 불투명 표면은 앞선 환경광 패스가 만든 화면 확산·반사 결과를 읽는다. 확산에는 현재 재질의 kD·albedo를 곱하고 반사는 그대로 더하며, 여기서 AO를 다시 곱하지 않는다.
            ambient=kD*albedo*environmentDiffuseTexture.sample(screenSampler,screenUV,level(0)).rgb;
            ambient+=environmentSpecularTexture.sample(screenSampler,screenUV,level(0)).rgb;
        }
    }
#endif

    float3 emissive = drawConstants.emissiveColor.rgb;
    if ((textureFlags & kTextureFlagEmissive) != 0u) // 발광 이미지가 있으면 발광 상수 RGB에 이미지를 곱한다.
    {
        emissive *=
            emissiveTexture.sample(materialSampler, input.uv).rgb;
    }

    float3 color = lighting.lightCounts.w > 0.5 ? ambient + directLight + emissive : albedo + emissive;
    if(lighting.pbrParams.z >= 0.0f) { // 직접 출력 경로만 노출과 톤 매핑을 적용한다. 음수는 선형 HDR로 남겨 후단 출력 패스에 맡긴다.
        color = min(color * lighting.pbrParams.w, 16777216.0f);
        color = color / (color + 1.0f);
        if (lighting.pbrParams.z > 0.5f) color = pow(color, float3(1.0f / 2.2f)); // 이 출력 경로가 감마 적용을 요구할 때만 RGB에 1/2.2 거듭제곱을 적용한다.
    }
    // HDR 장면의 RGBA16F 저장 한도 65504로 RGB를 제한하고 NaN은 0으로 치환한다. 넘친 밝기가 후속 GI·bloom·시간 누적 통계까지 오염되는 일을 줄인다.
    color=select(clamp(color,0.0f,65504.0f),float3(0),isnan(color));
    return float4(color, alphaMode==2u ? base.a : 1.0f);
}

// Metal 진입점이 texture·buffer·sampler 슬롯을 실제 인자로 연결한다. bindless면 재질 표의 두 uint4를 풀어 같은 RunFragmentShader에 5개 재질 텍스처를 넘긴다.
fragment float4 RENDERER_FRAGMENT_ENTRY(
    RasterData input [[stage_in]],
    bool frontFace [[front_facing]],
#if !RENDERER_BINDLESS // 개별 바인딩 경로에서만 명명된 재질·화면 환경광 자원을 포함한다. bindless 경로는 재질 배열을 사용한다.
    texture2d<float> screenAOTexture [[texture(16)]],
    texture2d<float> environmentSpecularTexture [[texture(13)]],
    texture2d<float> environmentDiffuseTexture [[texture(17)]],
    constant EnvironmentConstants& environment [[buffer(14)]],
    sampler screenSampler [[sampler(15)]],
#endif
#if RENDERER_BINDLESS // bindless 빌드이면 재질 배열·인덱스 표와 그에 맞는 그림자 연결 번호를 사용한다.
    array<texture2d<float>,19> materialTextures [[texture(0)]],
    const device uint4* materialTextureIndices [[buffer(29)]],
#endif
    constant DrawConstants& drawConstants [[buffer(RENDERER_BINDING_INLINE_CONSTANTS)]],
    constant RendererLightingConstants& lighting [[buffer(RENDERER_BINDING_LIGHTING_CONSTANTS)]],
    const device ClusterLight* clusterLights [[buffer(19)]],
    const device uint* clusterList [[buffer(20)]],
    constant ClusterParameters& clusters [[buffer(21)]],
    // LTC 변환 표는 텍스처 26, 진폭 표는 27이다. 재질 샘플러 8로 표의 mip 0을 읽으며 64×64 표의 텍셀 중심에 맞춘 UV를 사용한다.
    texture2d<float> ltcMatrixTexture [[texture(26)]],
    texture2d<float> ltcAmplitudeTexture [[texture(27)]],
#if !RENDERER_BINDLESS // 개별 바인딩 경로에서만 명명된 재질·화면 환경광 자원을 포함한다. bindless 경로는 재질 배열을 사용한다.
    texture2d<float> baseColorTexture [[texture(RENDERER_BINDING_BASE_COLOR_TEXTURE)]],
#endif
#if RENDERER_ENABLE_SHADOWS // 그림자 빌드에서만 깊이 자원·시점 자료·가시율 계산을 포함한다. 꺼진 직접광 경로의 가시율은 1이다.
    depth2d<float> shadowMap [[texture(RENDERER_BINDING_SHADOW_TEXTURE)]],
    constant ShadowMatrix& shadowMatrix [[buffer(RENDERER_BINDING_SHADOW_MATRIX)]],
#endif
#if !RENDERER_BINDLESS // 개별 바인딩 경로에서만 명명된 재질·화면 환경광 자원을 포함한다. bindless 경로는 재질 배열을 사용한다.
    texture2d<float> metallicRoughnessTexture [[texture(RENDERER_BINDING_METALLIC_ROUGHNESS_TEXTURE)]],
#endif
#if !RENDERER_BINDLESS // 개별 바인딩 경로에서만 명명된 재질·화면 환경광 자원을 포함한다. bindless 경로는 재질 배열을 사용한다.
    texture2d<float> normalTexture [[texture(RENDERER_BINDING_NORMAL_TEXTURE)]],
#endif
#if !RENDERER_BINDLESS // 개별 바인딩 경로에서만 명명된 재질·화면 환경광 자원을 포함한다. bindless 경로는 재질 배열을 사용한다.
    texture2d<float> occlusionTexture [[texture(RENDERER_BINDING_OCCLUSION_TEXTURE)]],
#endif
#if !RENDERER_BINDLESS // 개별 바인딩 경로에서만 명명된 재질·화면 환경광 자원을 포함한다. bindless 경로는 재질 배열을 사용한다.
    texture2d<float> emissiveTexture [[texture(RENDERER_BINDING_EMISSIVE_TEXTURE)]],
#endif
#if RENDERER_ENABLE_SHADOWS // 그림자 빌드에서만 깊이 자원·시점 자료·가시율 계산을 포함한다. 꺼진 직접광 경로의 가시율은 1이다.
    sampler shadowSampler [[sampler(RENDERER_BINDING_SHADOW_SAMPLER)]],
#endif
    sampler materialSampler [[sampler(RENDERER_BINDING_MATERIAL_SAMPLER)]])
{
#if RENDERER_BINDLESS // bindless 빌드이면 재질 배열·인덱스 표와 그에 맞는 그림자 연결 번호를 사용한다.
    // 재질 하나는 uint4 두 칸이다. 첫 칸 xyzw가 기본색·금속/거칠기·법선·AO, 둘째 칸 x가 발광 텍스처 인덱스다.
    const uint4 indicesA=materialTextureIndices[drawConstants.padding2*2];
    const uint4 indicesB=materialTextureIndices[drawConstants.padding2*2+1];
#define baseColorTexture materialTextures[indicesA[0]]
#define metallicRoughnessTexture materialTextures[indicesA[1]]
#define normalTexture materialTextures[indicesA[2]]
#define occlusionTexture materialTextures[indicesA[3]]
#define emissiveTexture materialTextures[indicesB.x]
#endif
    return RunFragmentShader(
        input,
        frontFace,
        drawConstants,
        lighting,
        clusterLights,clusterList,clusters,
        ltcMatrixTexture,ltcAmplitudeTexture,
        baseColorTexture,
#if RENDERER_ENABLE_SHADOWS // 그림자 빌드에서만 깊이 자원·시점 자료·가시율 계산을 포함한다. 꺼진 직접광 경로의 가시율은 1이다.
        shadowMap,
        shadowMatrix,
#endif
        metallicRoughnessTexture,
        normalTexture,
        occlusionTexture,
        emissiveTexture,
#if RENDERER_ENABLE_SHADOWS // 그림자 빌드에서만 깊이 자원·시점 자료·가시율 계산을 포함한다. 꺼진 직접광 경로의 가시율은 1이다.
        shadowSampler,
#endif
        materialSampler
#if !RENDERER_BINDLESS // 개별 바인딩 경로에서만 명명된 재질·화면 환경광 자원을 포함한다. bindless 경로는 재질 배열을 사용한다.
        ,screenAOTexture,environmentSpecularTexture,environmentDiffuseTexture,environment,screenSampler
#endif
        );
}
