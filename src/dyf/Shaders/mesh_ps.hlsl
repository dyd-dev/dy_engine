#ifndef RENDERER_BINDLESS // 재질 텍스처 배열 방식이 지정되지 않았으면 개별 텍스처 바인딩을 기본으로 사용한다.
#define RENDERER_BINDLESS 0
#endif
#ifndef RENDERER_DEPTH_PREPASS
#define RENDERER_DEPTH_PREPASS 0
#endif
#ifndef RENDERER_ENABLE_AREA_LIGHTS
#define RENDERER_ENABLE_AREA_LIGHTS 1
#endif
#define RENDERER_MAX_DIRECTIONAL_LIGHTS 4
#define RENDERER_MAX_POINT_LIGHTS 16
#define RENDERER_MAX_SPOT_LIGHTS 16
#define RENDERER_MAX_RECT_AREA_LIGHTS 4
#define RENDERER_MAX_DISC_AREA_LIGHTS 4

#define RENDERER_TEXTURE_FLAG_BASE_COLOR 1
#define RENDERER_TEXTURE_FLAG_METALLIC_ROUGHNESS 2
#define RENDERER_TEXTURE_FLAG_NORMAL 4
#define RENDERER_TEXTURE_FLAG_OCCLUSION 8
#define RENDERER_TEXTURE_FLAG_EMISSIVE 16
#define RENDERER_TEXTURE_FLAG_RECEIVE_SHADOW 32
#define RENDERER_TEXTURE_FLAG_BC5_NORMAL 256

#ifndef RENDERER_ENABLE_SHADOWS // 그림자 사용 여부를 파이프라인이 명시하지 않았으면 컴파일 오류를 내어 자원 배치 불일치를 막는다.
#error RENDERER_ENABLE_SHADOWS must be defined
#endif


// 한 draw의 공통 행렬과 재질 입력이다. 정점 단계와 192바이트 배치를 공유하며 draw 상수는 GLSL push constant, HLSL b10, Metal buffer 10으로 전달된다.
// 표면에 수직인 방향(법선)의 Y축 반전 여부를 padding1의 비트 값 8로 전달한다.
// padding0은 alphaCutoff의 float 비트, padding1은 하위 2비트 알파 모드·양면(4)·법선 Y 반전(8)·unlit(16)·반전 변환 면 방향 보정(32), padding2는 bindless 재질 표 번호다.
cbuffer DrawConstants : register(b10, space0)
{
    column_major float4x4 viewProjectionMatrix;
    column_major float4x4 modelMatrix;
    uint drawTextureFlags;
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
cbuffer RendererLighting : register(b1, space0) { RendererLightingConstants lighting; };
// 국소 광원의 위치/거리, RGB/강도, 스폿 방향/외곽 cos, 내부 cos·광원 종류·그림자 번호를 한 항목으로 묶는다.
// 연결 19의 광원 배열, 20의 군집 목록, 21의 군집 투영 상수를 함께 사용한다. GLSL은 set 0, HLSL은 t19/t20/b21, Metal은 buffer 19/20/21이다.
struct ClusterLight {float4 positionRange;float4 colorIntensity;float4 directionOuterCos;float4 coneShadow;};
StructuredBuffer<ClusterLight> clusterLights:register(t19,space0);
StructuredBuffer<uint> clusterList:register(t20,space0);
cbuffer ClusterParameters:register(b21,space0) {column_major float4x4 clusterView;column_major float4x4 clusterProjection;
    column_major float4x4 clusterInverseProjection;float4 clusterDepth;uint4 clusterDimensions;uint4 clusterCounts;};
// 월드 위치를 군집 카메라로 투영하여 화면 XY와 깊이 슬라이스를 구한다. 원근 카메라는 로그 깊이, 직교 카메라는 선형 깊이를 쓴다.
// 반환값은 개수 1칸과 광원 인덱스 목록이 이어진 군집 구간의 시작이다. 0xffffffff는 목록을 사용할 수 없다는 표시이며 전체 국소 광원 순회로 대체한다.
uint ClusterBase(float3 position)
{
    if(clusterCounts.y==0)return 0xffffffffu; // 군집 목록 사용이 꺼져 있으면 전체 국소 광원 순회 표식을 반환한다.
    float4 viewPosition=mul(clusterView,float4(position,1)),clip=mul(clusterProjection,viewPosition);
    float distance=-viewPosition.z;
    if(distance<clusterDepth.x || distance>clusterDepth.y || clip.w<=0)return 0xffffffffu; // 군집 깊이 범위 밖이거나 투영 뒤쪽이면 유효한 군집을 고를 수 없다.
    float2 coordinate=clip.xy/clip.w*.5+.5;
    if(any(coordinate<0) || any(coordinate>=1))return 0xffffffffu; // 군집 화면 범위 [0,1) 밖이면 전체 국소 광원 경로로 돌린다.
    float slice=clusterCounts.z!=0?log(distance/clusterDepth.x)/clusterDepth.z:
        (distance-clusterDepth.x)/(clusterDepth.y-clusterDepth.x);
    uint3 index=min(uint3(coordinate*float2(clusterDimensions.xy),slice*float(clusterDimensions.z)),clusterDimensions.xyz-uint3(1,1,1));
    return (index.x+clusterDimensions.x*(index.y+clusterDimensions.y*index.z))*(clusterDimensions.w+1);
}

#if !RENDERER_BINDLESS // 개별 바인딩 경로에서만 명명된 재질 텍스처를 포함한다. bindless 경로는 재질 배열을 사용한다.
Texture2D BaseColorTexture : register(t0, space0);
Texture2D MetallicRoughnessTexture : register(t4, space0);
Texture2D NormalTexture : register(t5, space0);
Texture2D OcclusionTexture : register(t6, space0);
Texture2D EmissiveTexture : register(t7, space0);
#endif
#if RENDERER_ENABLE_SHADOWS // 그림자 빌드에서만 깊이 자원·시점 자료·가시율 계산을 포함한다. 꺼진 직접광 경로의 가시율은 1이다.
#if RENDERER_BINDLESS // bindless 빌드이면 재질 배열·인덱스 표와 그에 맞는 그림자 연결 번호를 사용한다.
Texture2D ShadowMap : register(t28, space0);
#else
Texture2D ShadowMap : register(t2, space0);
#endif
#endif
SamplerState LinearSampler : register(s8, space0);
// LTC 변환 표는 텍스처 26, 진폭 표는 27이다. 재질 샘플러 8로 표의 mip 0을 읽으며 64×64 표의 텍셀 중심에 맞춘 UV를 사용한다.
Texture2D<float4> ltcMatrixTexture:register(t26,space0);
Texture2D<float4> ltcAmplitudeTexture:register(t27,space0);
#if RENDERER_BINDLESS
Texture2D<float4> ScreenAOTexture : register(t22,space0);
Texture2D<float4> EnvironmentSpecularTexture : register(t23,space0);
Texture2D<float4> EnvironmentDiffuseTexture : register(t24,space0);
#else
Texture2D<float4> ScreenAOTexture : register(t16,space0);
Texture2D<float4> EnvironmentSpecularTexture : register(t13,space0);
Texture2D<float4> EnvironmentDiffuseTexture : register(t17,space0);
#endif
SamplerState ScreenSampler : register(s15,space0);
// 환경 상수는 b14이며 bindless 경로도 같은 배치를 사용한다.
// environmentSettings.x/y/z는 환경 사용·강도·회전, environmentScreen.xy/z/w는 화면 역크기·AO 사용·환경 기능 비트다.
// environmentSH 9개와 probe 정보도 CPU와 같은 메모리 배치를 사용한다.
// Basic bindless rendering supplies neutral settings.x=0 and screen.z=0.
cbuffer EnvironmentConstants : register(b14,space0)
{
    float4 environmentSH[9];
    float4 environmentSettings;
    float4 environmentScreen;
    float4 environmentProbePosition;
    float4 environmentProbeBoxMin;
    float4 environmentProbeBoxMax;
};
// 법선의 XY를 환경 회전만큼 돌리고 9개 구면조화 계수로 확산 입사광을 복원한다. 음수 결과는 버리며, 호출측이 albedo·1/pi·환경 강도를 곱한다.
float3 EnvironmentIrradiance(float3 n)
{
    float c=cos(environmentSettings.z),s=sin(environmentSettings.z);
    n.xy=float2(c*n.x+s*n.y,-s*n.x+c*n.y);
    float3 value=environmentSH[0].rgb*0.28209479177387814+
        environmentSH[1].rgb*(0.4886025119029199*n.y)+environmentSH[2].rgb*(0.4886025119029199*n.z)+
        environmentSH[3].rgb*(0.4886025119029199*n.x)+environmentSH[4].rgb*(1.0925484305920792*n.x*n.y)+
        environmentSH[5].rgb*(1.0925484305920792*n.y*n.z)+environmentSH[6].rgb*(0.31539156525252005*(3*n.z*n.z-1))+
        environmentSH[7].rgb*(1.0925484305920792*n.x*n.z)+environmentSH[8].rgb*(0.5462742152960396*(n.x*n.x-n.y*n.y));
    return max(value,0);
}
#if RENDERER_ENABLE_SHADOWS
SamplerState ShadowSampler : register(s9, space0);
#endif

// 표면을 따라가는 방향(접선)을 정점에서 받는다.
// UV·월드 위치·법선·접선·정점 색상을 픽셀 위치로 보간하고, SV_IsFrontFace로 받은 면 앞뒤 정보를 양면 재질에 반영한다.
struct PSInput
{
    float4 position           : SV_POSITION;
    float2 uv                 : TEXCOORD0;
    float3 worldPosition      : TEXCOORD1;
    float3 worldNormal        : TEXCOORD2;
    float4 worldTangent       : TEXCOORD3;
    float4 color              : TEXCOORD5;
#if RENDERER_ENABLE_SHADOWS
    float4 lightSpacePosition : TEXCOORD4;
#endif
};

static const float PI = 3.14159265359;

// GGX 분포는 미리 계산한 거칠기의 네제곱 a2를 받는다. 외적으로 1-(N·H)^2의 상쇄 오차를 줄인다.
float DistributionGGX(float3 normal, float3 halfway, float a2)
{
    float ndoth = clamp(dot(normal, halfway), 0.0, 1.0);
    float3 crossNH = cross(normal, halfway);
    float denom = dot(crossNH, crossNH) + ndoth * ndoth * a2;
    return a2 / (PI * denom * denom);
}

// 직접광의 미세면 가림을 근사한다. k=(roughness+1)^2/8은 표면마다 한 번 계산해 공유한다.
float GeometrySchlickGGX(float ndotv, float k)
{
    return ndotv / max(ndotv * (1.0 - k) + k, 0.0001);
}

float SchlickPow5(float base)
{
    float base2=base*base;
    return (base2*base2)*base;
}

// 정면 반사율에서 시작해 비스듬히 볼수록 반사가 커지는 Fresnel 효과를 5제곱으로 근사한다.
float3 FresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0 - f0) * SchlickPow5(saturate(1.0 - cosTheta));
}

// 주변광과 면광원 확산 가중치용 Fresnel 근사다. 거친 표면에서 가장자리 반사가 과해지지 않도록 roughness로 상한을 조절한다.
float3 FresnelSchlickRoughness(float cosTheta, float3 f0, float roughness)
{
    float3 roughnessF0 = max(float3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness), f0);
    return f0 + (roughnessF0 - f0) * SchlickPow5(saturate(1.0 - cosTheta));
}

#if RENDERER_BINDLESS // bindless 빌드이면 재질 배열·인덱스 표와 그에 맞는 그림자 연결 번호를 사용한다.
Texture2D materialTextures[19]:register(t0, space0);
StructuredBuffer<uint4> materialTextureIndices:register(t29, space0);
#endif
// kind 0~4는 기본색·금속/거칠기·법선·AO·발광이다. bindless 경로는 재질당 uint4 두 칸 중 kind/4번 칸의 kind%4 성분을 읽어 19개 텍스처 배열의 인덱스로 사용한다.
// 개별 경로는 0/4/5/6/7번 텍스처와 8번 샘플러를 사용한다. 텍스처의 sRGB/Linear 해석은 연결된 텍스처 형식이 담당한다.
float4 SampleMaterial(uint kind,float2 uv)
{
#if RENDERER_BINDLESS // bindless 빌드이면 재질 배열·인덱스 표와 그에 맞는 그림자 연결 번호를 사용한다.
    uint index=materialTextureIndices[padding2*2u+kind/4u][kind%4u];
    return materialTextures[index].Sample(LinearSampler,uv);
#else
    if(kind==0u) return BaseColorTexture.Sample(LinearSampler,uv); // 기본색 종류는 0번 텍스처를 읽는다.
    if(kind==1u) return MetallicRoughnessTexture.Sample(LinearSampler,uv); // 금속/거칠기 종류는 4번 텍스처를 읽는다.
    if(kind==2u) return NormalTexture.Sample(LinearSampler,uv); // 법선 종류는 5번 텍스처를 읽는다.
    if(kind==3u) return OcclusionTexture.Sample(LinearSampler,uv); // AO 종류는 6번 텍스처를 읽고, 남은 발광 종류는 아래 7번 텍스처로 간다.
    return EmissiveTexture.Sample(LinearSampler,uv);
#endif
}

// 보간 법선을 정규화하고 접선을 법선에 직교화해 TBN 기준축을 만든다. 법선 맵을 -1~1로 바꾸고 XY 강도·선택적 Y 반전을 적용해 월드 법선으로 옮긴다.
float3 GetNormal(PSInput input, uint textureFlags)
{
    float3 normal = normalize(input.worldNormal);
    float normalScale = materialParams.z;
    if ((textureFlags & RENDERER_TEXTURE_FLAG_NORMAL) == 0u || abs(normalScale) <= 0.0001) // 법선 이미지가 없거나 법선 강도가 사실상 0이면 보간된 월드 법선을 그대로 사용한다.
    {
        return normal;
    }
    float3 tangent = normalize(input.worldTangent.xyz - normal * dot(normal, input.worldTangent.xyz));
    float3 bitangent = normalize(cross(normal, tangent)) * input.worldTangent.w;
    // GLSL·Metal은 T/B/N을 열로 놓고 행렬×법선을 계산한다. HLSL은 생성자의 행으로 놓은 뒤 법선×행렬을 계산하여 같은 월드 방향을 얻는다.
    float3x3 tbn = float3x3(tangent, bitangent, normal);
    float3 tangentNormal = SampleMaterial(2u,input.uv).xyz * 2.0 - 1.0;
    if((textureFlags & RENDERER_TEXTURE_FLAG_BC5_NORMAL)!=0u)
        tangentNormal.z=sqrt(max(0.0f,1.0f-dot(tangentNormal.xy,tangentNormal.xy)));
    tangentNormal.xy *= normalScale;
    if((padding1 & 8u)!=0u) tangentNormal.y=-tangentNormal.y; // 법선 맵의 Y 방향이 반대인 재질이면 접선 공간 Y 성분만 뒤집는다.
    return normalize(mul(tangentNormal, tbn));
}

#if RENDERER_ENABLE_SHADOWS // 그림자 빌드에서만 깊이 자원·시점 자료·가시율 계산을 포함한다. 꺼진 직접광 경로의 가시율은 1이다.
// 광원별 시작 시점·개수·강도·광원 크기, 캐스케이드 경계, 아틀라스 영역과 PCSS용 투영 계수를 공유한다. 그림자 좌표는 선택한 시점 행렬로 월드 위치를 다시 투영해 계산한다.
struct ShadowData {
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
cbuffer ShadowMatrices:register(b3, space0) {ShadowData shadowMatrix;};

int2 DyShadowDimensions()
{
    uint width,height;ShadowMap.GetDimensions(width,height);return int2(width,height);
}
#define DY_SHADOW_PROJECTION(index) shadowMatrix.viewProjectionParams[index]
#define DY_SHADOW_MATRIX(index) shadowMatrix.lightViewProjectionMatrix[index]
#define DY_SHADOW_TILE(index) shadowMatrix.atlasRect[index]
#define DY_SHADOW_DIMENSIONS DyShadowDimensions()
#define DY_SHADOW_READ(pixel) ShadowMap.Load(int3(pixel,0)).r
#define DY_SHADOW_FILTER shadowMatrix.filterParams
#define DY_SHADOW_SOFT shadowMatrix.softShadowParams
// 현재 자원 읽기 함수를 DY_SHADOW 매크로로 연결한 뒤 공통 PCSS 구현을 포함한다. 같은 필터 알고리즘을 언어별 타입과 바인딩에 맞춰 쓴다.
#include "shadow_filter.hlsl"

// 한 그림자 시점의 가시율을 구한다. 0~1 깊이와 위아래를 뒤집은 UV를 아틀라스 영역에 넣고, 같은 표면이 자기 그림자로 판정되는 오차를 줄이도록 표면 기울기 기반 bias를 수신 깊이에서 뺀다.
// 광원 크기가 있으면 PCSS로 반그림자를, 없으면 고정 텍셀 반경 PCF로 가시 표본 비율을 구한다. strength는 최종 어두워지는 양만 조절한다.
float SampleShadowView(int index,float3 worldPosition,float3 normal,float3 lightDir,float strength,float sourceRadius,int pointBase)
{
    float4 clip=mul(shadowMatrix.lightViewProjectionMatrix[index],float4(worldPosition,1.0));
    if(clip.w<=0.0)return 1.0; // 선택한 광원 시점 뒤쪽이면 해당 시점 그림자를 적용하지 않는다.
    float3 ndc=clip.xyz/clip.w;
    float2 uv=ndc.xy*.5+.5;
    uv.y=1.0-uv.y;
    if(uv.x<0.0||uv.y<0.0||uv.x>1.0||uv.y>1.0||ndc.z<0.0||ndc.z>1.0)return 1.0; // 그림자 시점의 UV 또는 0~1 깊이 범위 밖이면 완전 가시로 둔다.
    float4 tile=shadowMatrix.atlasRect[index];
    uint width,height;ShadowMap.GetDimensions(width,height);float2 texel=1.0/float2(width,height);
    float2 minUv=tile.xy+texel*.5;
    float2 maxUv=tile.xy+tile.zw-texel*.5;
    float2 localUv=uv;
    uv=tile.xy+uv*tile.zw;
    float angle=1.0-max(dot(normal,lightDir),0.0);
    float bias=max(lighting.shadowParams.x,lighting.shadowParams.y*angle)+lighting.shadowParams.z*angle;
    float receiver=ndc.z-bias;
    if(sourceRadius>0 && shadowMatrix.softShadowParams.x>0) // 광원 크기와 허용 소프트 반경이 모두 양수일 때만 PCSS를 사용한다.
        return 1-clamp(strength,0.0,1.0)*(1-DyShadowPcss(index,pointBase,localUv,receiver,clip.w,lightDir,sourceRadius));
    float radius=min(lighting.shadowParams.w,shadowMatrix.filterParams.z);
    int extent=int(ceil(radius));
    float visible=0.0,samples=0.0;
    for(int y=-extent;y<=extent;++y)for(int x=-extent;x<=extent;++x)
    {
        float2 sampleUv=clamp(uv+float2(x,y)*texel,minUv,maxUv);
        visible+=receiver<=ShadowMap.SampleLevel(ShadowSampler,sampleUv,0).r?1.0:0.0;
        samples+=1.0;
    }
    return 1.0-clamp(strength,0.0,1.0)*(1.0-visible/max(samples,1.0));
}

// 방향광은 카메라 깊이의 캐스케이드, 점광원은 광원→표면 방향의 큐브면, 스폿은 해당 시점을 고른다. 방향광 경계 마지막 10%에서는 다음 캐스케이드와 섞는다.
float SampleShadow(int type,int lightIndex,float3 worldPosition,float3 normal,float3 lightDir)
{
    if((drawTextureFlags & RENDERER_TEXTURE_FLAG_RECEIVE_SHADOW)==0u)return 1.0; // 객체의 그림자 받기 비트가 꺼져 있으면 가시율 1로 조명을 그대로 받는다.
    float4 views=type==1?shadowMatrix.directionalViews[lightIndex]:(type==2?shadowMatrix.pointViews[lightIndex]:shadowMatrix.spotViews[lightIndex]);
    int count=int(views.y+.5);
    if(count==0)return 1.0; // 광원에 배정된 그림자 시점이 없으면 가시율 1을 반환한다.
    int local=0;
    float depth=0.0;
    if(type==1) // 방향광이면 카메라 앞쪽 거리로 캐스케이드를 선택한다.
    {
        depth=-mul(shadowMatrix.cameraView,float4(worldPosition,1.0)).z;
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
    float visibility=SampleShadowView(index,worldPosition,normal,lightDir,views.z,views.w,type==2?int(views.x+.5):-1);
    if(type==1 && local<count-1) // 방향광의 마지막 캐스케이드가 아니면 현재 거리 구간의 끝 10%에서 다음 시점과 섞을 비율을 계산한다.
    {
        float previous=local==0?shadowMatrix.filterParams.w:shadowMatrix.directionalSplits[lightIndex][local-1];
        float split=shadowMatrix.directionalSplits[lightIndex][local];
        float blend=smoothstep(lerp(previous,split,.9),split,depth);
        if(blend>0.0)visibility=lerp(visibility,SampleShadowView(index+1,worldPosition,normal,lightDir,views.z,views.w,type==2?int(views.x+.5):-1),blend); // 캐스케이드 경계의 혼합 구간에서만 다음 시점 그림자도 읽는다.
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

// 같은 픽셀의 모든 직접광에 공통인 Fresnel·거칠기·시선 항을 준비한다.
struct DirectSurface
{
    float3 f0;
    float ndotv,alphaSquared,k,viewGeometry,diffuseFactor;
};
inline DirectSurface PrepareDirectSurface(float3 normal,float3 viewDir,float3 albedo,float metallic,float roughness)
{
    DirectSurface surface;
    surface.f0=lerp(float3(.04,.04,.04),albedo,metallic);
    surface.ndotv=max(dot(normal,viewDir),0.0);
    float a=roughness*roughness,r=roughness+1.0;
    surface.alphaSquared=a*a;
    surface.k=(r*r)/8.0;
    surface.viewGeometry=GeometrySchlickGGX(surface.ndotv,surface.k);
    surface.diffuseFactor=1.0-metallic;
    return surface;
}
inline float3 EvaluateDirectLight(
    float3 normal,
    float3 viewDir,
    float3 lightDir,
    float3 incidentIlluminance,
    float3 albedo,
    DirectSurface surface,
    float visibility) {
    float ndotl = max(dot(normal, lightDir), 0.0);
    float ndotv = surface.ndotv;
    if (ndotl <= 0.0 || ndotv <= 0.0 || visibility <= 0.0) {
        return float3(0.0,0.0,0.0);
    }
    float3 halfwayVector = viewDir + lightDir;
    float halfwayLengthSquared = dot(halfwayVector, halfwayVector);
    if (halfwayLengthSquared <= 0.000001) { // 시선과 광원 방향이 상쇄되어 반각 벡터를 정규화할 수 없으면 0을 반환한다.
        return float3(0.0,0.0,0.0);
    }
    float3 halfway = halfwayVector * rsqrt(halfwayLengthSquared);
    float ndf = DistributionGGX(normal, halfway, surface.alphaSquared);
    float geometry = surface.viewGeometry * GeometrySchlickGGX(ndotl, surface.k);
    float3 fresnel = FresnelSchlick(max(dot(halfway, viewDir), 0.0), surface.f0);
    float3 specular = (ndf * geometry * fresnel) / max(4.0 * ndotv * ndotl, 0.0001);
    float3 diffuseWeight = (float3(1.0,1.0,1.0) - fresnel) * surface.diffuseFactor;
    return (diffuseWeight * albedo / PI + specular) * incidentIlluminance * ndotl * visibility;
}

#if RENDERER_ENABLE_AREA_LIGHTS
// LTC 적분과 표 배치는 Heitz 외 SIGGRAPH 2016 및 Assets/ltc/LICENSE를 따른다.
// 정규화된 두 경계 방향 사이의 구면 모서리 적분을 계산한다. 모서리들을 순서대로 더해 면광원이 차지하는 cosine 가중 영역을 얻는다.
inline float LtcEdgeIntegral(float3 a,float3 b)
{
    float3 edge=cross(a,b);
    float sineSquared=dot(edge,edge),cosine=clamp(dot(a,b),-1.0,1.0);
    float limit=.25*cosine;
    if(cosine>=.5 && sineSquared<=limit*limit)
    {
        // The polynomial uses squared sine; avoid taking its square root just to square it again.
        float invCos=1.0/cosine,z=(sineSquared*invCos)*invCos;
        // atan(r)/r, 0<=r<=1/4: the omitted term is <=r^12/13<4.59e-9.
        float p=1.0+z*(-1.0/3.0+z*(1.0/5.0+z*(-1.0/7.0+z*(1.0/9.0-z/11.0))));
        float angleOverSine=p*invCos;
        if(sineSquared<=1e-24)
        {
            float sine=length(edge);
            if(sine<1e-12)angleOverSine*=sine/1e-12;
        }
        return edge.z*angleOverSine*0.15915494309189535;
    }
    float sine=length(edge);
    return edge.z*(atan2(sine,cosine)/max(sine,1e-12))*0.15915494309189535;
}
inline float3 LtcProject(float3 offset,float3 tangent,float3 bitangent,float3 normal)
{
    return float3(dot(offset,tangent),dot(offset,bitangent),dot(offset,normal));
}
inline float3 LtcTransform(float3 local,float4 inverseMatrix)
{
    return float3(inverseMatrix.x*local.x+inverseMatrix.z*local.z,local.y,
        inverseMatrix.y*local.x+inverseMatrix.w*local.z);
}
// 표면 기준축과 64×64 LTC 변환·진폭 표를 한 번 읽어 사각·원판 광원의 적분에 공유한다.
struct LtcSurface
{
    float3 normal,tangent,bitangent;
    float4 inverseMatrix;
    float3 specularWeight,diffuseWeight;
};
inline LtcSurface PrepareLtcSurface(float3 normal,float3 viewDir,DirectSurface directSurface,float roughness)
{
    LtcSurface surface;
    surface.normal=normal;
    surface.tangent=viewDir-normal*dot(normal,viewDir);
    if(dot(surface.tangent,surface.tangent)<1e-8)
        surface.tangent=cross(abs(normal.z)<.999?float3(0,0,1):float3(0,1,0),normal);
    surface.tangent=normalize(surface.tangent);
    surface.bitangent=cross(normal,surface.tangent);
    float ndotv=clamp(directSurface.ndotv,0.0,1.0);
    float2 coordinate=(float2(clamp(roughness,0.0,1.0),sqrt(1-ndotv))*63.0+.5)/64.0;
    surface.inverseMatrix=ltcMatrixTexture.SampleLevel(LinearSampler,coordinate,0);
    float4 amplitude=ltcAmplitudeTexture.SampleLevel(LinearSampler,coordinate,0);
    float3 f0=directSurface.f0;
    surface.specularWeight=f0*max(amplitude.x,0.0)+(float3(1,1,1)-f0)*max(amplitude.y,0.0);
    surface.diffuseWeight=(float3(1,1,1)-FresnelSchlickRoughness(ndotv,f0,roughness))*directSurface.diffuseFactor;
    return surface;
}
// Original float(corner)*float(pi/8) angles; retain their small axis asymmetries.
static const float2 LtcDiscDirections[16]={
    float2(1.0,0.0),float2(.9238795042,.3826834559),
    float2(.7071067691,.7071067691),float2(.3826834261,.9238795042),
    float2(-4.371138829e-8,1.0),float2(-.3826835155,.9238795042),
    float2(-.7071067691,.7071067691),float2(-.9238796234,.3826832771),
    float2(-1.0,-8.742277657e-8),float2(-.9238795042,-.3826834261),
    float2(-.7071066499,-.7071068883),float2(-.3826831281,-.9238796830),
    float2(1.192488064e-8,-1.0),float2(.3826836050,-.9238794446),
    float2(.7071070075,-.7071065307),float2(.9238795638,-.3826834261)};
inline float3 LtcPolygonPoint(float3 center,float3 axisX,float3 axisY,int corner,bool disc)
{
    float2 coordinate=disc?LtcDiscDirections[corner]:
        float2(corner==0 || corner==3?-1.0:1.0,corner<2?-1.0:1.0);
    return center+axisX*coordinate.x+axisY*coordinate.y;
}
inline void LtcAppendPoint(float3 clippedPoint,inout float3 first,inout float3 last,inout bool haveVertex,inout float sum)
{
    float squared=dot(clippedPoint,clippedPoint);
    if(squared<=1e-20)return;
    clippedPoint*=rsqrt(squared);
    if(haveVertex)sum+=LtcEdgeIntegral(last,clippedPoint);else {first=clippedPoint;haveVertex=true;}
    last=clippedPoint;
}
inline bool LtcPolygonAboveHorizon(float3 center,float3 axisX,float3 axisY,bool disc)
{
    float3 magnitude=abs(center)+abs(axisX)+abs(axisY);
    // Bound reconstructed corner lengths away from floating-point overflow.
    if(!all(isfinite(magnitude)) || any(magnitude>1e18))return false;
    float radius=disc?length(float2(axisX.z,axisY.z)):abs(axisX.z)+abs(axisY.z);
    float lower=center.z-radius;
    float margin=1e-5*max(1.0,abs(center.z)+abs(axisX.z)+abs(axisY.z));
    return isfinite(lower) && isfinite(margin) && lower>margin+1e-10;
}
inline float LtcPolygonIntegral(float4 inverseMatrix,float3 center,float3 axisX,float3 axisY,bool disc)
{
    float3 transformedCenter=LtcTransform(center,inverseMatrix);
    float3 transformedX=LtcTransform(axisX,inverseMatrix);
    float3 transformedY=LtcTransform(axisY,inverseMatrix);
    int count=disc?16:4;
    if(LtcPolygonAboveHorizon(transformedCenter,transformedX,transformedY,disc))
    {
        float3 firstFull=LtcPolygonPoint(transformedCenter,transformedX,transformedY,0,disc);
        firstFull*=rsqrt(dot(firstFull,firstFull));
        float3 previousFull=firstFull;float fullSum=0;
        for(int edge=1;edge<count;++edge)
        {
            float3 currentFull=LtcPolygonPoint(transformedCenter,transformedX,transformedY,edge,disc);
            currentFull*=rsqrt(dot(currentFull,currentFull));
            fullSum+=LtcEdgeIntegral(previousFull,currentFull);
            previousFull=currentFull;
        }
        fullSum+=LtcEdgeIntegral(previousFull,firstFull);
        return clamp(abs(fullSum),0.0,1.0);
    }
    float3 previous=LtcPolygonPoint(transformedCenter,transformedX,transformedY,count-1,disc);
    float3 first=float3(0,0,0),last=float3(0,0,0);bool haveVertex=false;float sum=0;
    for(int edge=0;edge<count;++edge)
    {
        float3 current=LtcPolygonPoint(transformedCenter,transformedX,transformedY,edge,disc);
        bool previousInside=previous.z>=0,currentInside=current.z>=0;
        // z=0 반구로 다각형을 자르되 정점 배열 대신 경계 순서대로 적분한다.
        if(previousInside!=currentInside)
        {
            float3 clippedPoint=lerp(previous,current,clamp(previous.z/(previous.z-current.z),0.0,1.0));
            clippedPoint.z=0;
            LtcAppendPoint(clippedPoint,first,last,haveVertex,sum);
        }
        if(currentInside)LtcAppendPoint(current,first,last,haveVertex,sum);
        previous=current;
    }
    if(haveVertex)sum+=LtcEdgeIntegral(last,first); // 점이 하나라도 있으면 마지막 점에서 첫 점으로 경계를 닫는다.
    return clamp(abs(sum),0.0,1.0);
}
inline bool LtcBelowHorizon(LtcSurface surface,float3 position,float3 center,float3 axisX,float3 axisY,
    float tangentCoefficient,float normalCoefficient,bool disc)
{
    float3 row=tangentCoefficient*surface.tangent+normalCoefficient*surface.normal;
    float centerZ=dot(center-position,row),x=dot(axisX,row),y=dot(axisY,row);
    float radius=disc?sqrt(x*x+y*y):abs(x)+abs(y);
    // Include uncancelled world coordinates and both basis terms in the rounding allowance.
    float3 magnitude=abs(center)+abs(position)+abs(axisX)+abs(axisY);
    float scale=abs(tangentCoefficient)*dot(magnitude,abs(surface.tangent))+
        abs(normalCoefficient)*dot(magnitude,abs(surface.normal));
    float bound=centerZ+radius,margin=1e-5*max(1.0,scale);
    return isfinite(bound) && isfinite(margin) && bound < -margin;
}
inline uint LtcIntegralMask(LtcSurface surface,float3 albedo,float3 position,float3 center,
    float3 axisX,float3 axisY,bool disc)
{
    uint mask=0u;
    if(any(surface.specularWeight!=0.0) &&
        !LtcBelowHorizon(surface,position,center,axisX,axisY,surface.inverseMatrix.y,surface.inverseMatrix.w,disc))
        mask|=1u;
    if(any(surface.diffuseWeight*albedo!=0.0) &&
        !LtcBelowHorizon(surface,position,center,axisX,axisY,0.0,1.0,disc))
        mask|=2u;
    return mask;
}
inline float3 EvaluateLtcPolygon(LtcSurface surface,float3 position,float3 center,float3 axisX,float3 axisY,
    bool disc,uint integralMask,float3 albedo,float3 radiance)
{
    // Both integrals share the surface-basis projection and retain their own inverse map.
    float3 localCenter=LtcProject(center-position,surface.tangent,surface.bitangent,surface.normal);
    float3 localX=LtcProject(axisX,surface.tangent,surface.bitangent,surface.normal);
    float3 localY=LtcProject(axisY,surface.tangent,surface.bitangent,surface.normal);
    float specular=0,diffuse=0;
    if((integralMask&1u)!=0u)specular=LtcPolygonIntegral(surface.inverseMatrix,localCenter,localX,localY,disc);
    if((integralMask&2u)!=0u)diffuse=LtcPolygonIntegral(float4(1,0,0,1),localCenter,localX,localY,disc);
    // 정규화된 cosine 적분이므로 diffuse에 1/pi나 거리 감쇠를 다시 곱하지 않는다.
    return radiance*(surface.specularWeight*specular+surface.diffuseWeight*albedo*diffuse);
}
// 방출 방향과 up으로 사각형 축을 만들고 네 모서리를 적분한다. 발광면 뒤쪽은 조명하지 않는다.
inline float3 EvaluateRectAreaLight(RendererRectAreaLight light,LtcSurface surface,float3 albedo,float3 worldPosition)
{
    float3 direction=normalize(light.directionWidth.xyz);
    if(light.directionWidth.w<=0 || light.upHeight.w<=0 || dot(direction,worldPosition-light.positionIntensity.xyz)<=0) // 면적이 없거나 사각 광원의 뒷면에 있는 수신점은 조명하지 않는다.
        return float3(0,0,0);
    float3 right=normalize(cross(direction,light.upHeight.xyz));
    float3 up=normalize(cross(right,direction));
    float3 x=right*(light.directionWidth.w*.5),y=up*(light.upHeight.w*.5),center=light.positionIntensity.xyz;
    uint integralMask=LtcIntegralMask(surface,albedo,worldPosition,center,x,y,false);
    if(integralMask==0u)return float3(0,0,0);
    return EvaluateLtcPolygon(surface,worldPosition,center,x,y,false,integralMask,albedo,
        light.color.rgb*max(light.positionIntensity.w,0.0));
}
// 원판 경계를 16각형으로 근사해 면 전체를 LTC 적분한다. 점광원 16개의 합으로 계산하지 않는다.
inline float3 EvaluateDiscAreaLight(RendererDiscAreaLight light,LtcSurface surface,float3 albedo,float3 worldPosition)
{
    float3 direction=normalize(light.directionRadius.xyz);
    if(light.directionRadius.w<=0 || dot(direction,worldPosition-light.positionIntensity.xyz)<=0)return float3(0,0,0); // 반지름이 없거나 원판 광원의 뒷면에 있는 수신점은 조명하지 않는다.
    float3 right=normalize(cross(direction,light.up.xyz));
    float3 up=normalize(cross(right,direction));
    float3 x=right*light.directionRadius.w,y=up*light.directionRadius.w;
    uint integralMask=LtcIntegralMask(surface,albedo,worldPosition,light.positionIntensity.xyz,x,y,true);
    if(integralMask==0u)return float3(0,0,0);
    // Keep the original 16-sided disc boundary and stream it in LTC space.
    return EvaluateLtcPolygon(surface,worldPosition,light.positionIntensity.xyz,x,y,true,integralMask,albedo,
        light.color.rgb*max(light.positionIntensity.w,0.0));
}
#endif

// 방향광 → 군집의 점/스폿 광원 → 사각·원판 면광원 순서로 직접광을 누적한다. 군집을 쓸 수 없거나 목록이 overflow 표시면 전체 국소 광원을 검사한다.
inline float3 EvaluateLights(RendererLightingConstants lighting, float3 worldPosition, float3 normal, float3 viewDir,
    float3 albedo, float roughness,DirectSurface directSurface) {
    float3 directLight = float3(0.0,0.0,0.0);
    float ndotv=directSurface.ndotv;
    int directionalCount = clamp(int(lighting.lightCounts.x + 0.5), 0, RENDERER_MAX_DIRECTIONAL_LIGHTS);
    for (int lightIndex = 0; lightIndex < directionalCount; ++lightIndex) {
        RendererDirectionalLight light = lighting.directionalLights[lightIndex];
        float3 lightDir = normalize(light.directionIntensity.xyz);
        // Match EvaluateDirectLight's zero result before sampling its shadow.
        if(ndotv<=0.0 || max(dot(normal,lightDir),0.0)<=0.0)continue;
#if RENDERER_ENABLE_SHADOWS
        float visibility = SampleShadow(1,lightIndex,worldPosition,normal,lightDir);
#else
        float visibility = 1.0;
#endif
        directLight += EvaluateDirectLight(normal, viewDir, lightDir,
            light.color.rgb * max(light.directionIntensity.w, 0.0), albedo, directSurface, visibility);
    }
    uint base=ClusterBase(worldPosition);
    uint listed=0xffffffffu;
    if(base!=0xffffffffu)listed=clusterList[base]; // 유효한 군집 주소일 때만 목록 개수를 읽는다. 무효 주소를 버퍼 인덱스로 쓰지 않는다.
    // 목록의 첫 칸이 0xffffffff이면 누락 없이 전체 clusterLights를 순회한다. 정상 목록은 해당 군집에 들어간 인덱스만 순회한다.
    uint localCount=listed==0xffffffffu?clusterCounts.x:listed;
    for(uint item=0u;item<localCount;++item)
    {
        uint lightIndex=listed==0xffffffffu?item:clusterList[base+1u+item];
        ClusterLight light=clusterLights[lightIndex];
        float3 toLight=light.positionRange.xyz-worldPosition;
        float distanceToLight=length(toLight);
        if(distanceToLight<=0.0001 || distanceToLight>=light.positionRange.w)continue; // 광원과 거의 같은 위치이거나 거리 범위 밖인 국소 광원은 건너뛴다.
        float3 lightDir=toLight/distanceToLight;
        if(ndotv<=0.0 || max(dot(normal,lightDir),0.0)<=0.0)continue;
        float attenuation=PunctualAttenuation(distanceToLight,light.positionRange.w);
        int type=int(light.coneShadow.y+0.5),shadowIndex=int(light.coneShadow.z);
        if(type==3)attenuation*=SpotConeAttenuation(dot(-lightDir,light.directionOuterCos.xyz), // 스폿 광원은 거리 감쇠에 원뿔 각도 감쇠를 추가한다.
            light.coneShadow.x,light.directionOuterCos.w);
        if(attenuation<=0.0)continue;
        float3 incidentIlluminance=light.colorIntensity.rgb*max(light.colorIntensity.w,0.0)*attenuation;
#if RENDERER_ENABLE_SHADOWS
        float visibility=shadowIndex>=0?SampleShadow(type,shadowIndex,worldPosition,normal,lightDir):1.0;
#else
        float visibility=1.0;
#endif
        directLight+=EvaluateDirectLight(normal,viewDir,lightDir,incidentIlluminance,albedo,directSurface,visibility);
    }
#if RENDERER_ENABLE_AREA_LIGHTS
    int rectAreaCount = clamp(int(lighting.areaLightCounts.x + 0.5), 0, RENDERER_MAX_RECT_AREA_LIGHTS);
    int discAreaCount = clamp(int(lighting.areaLightCounts.y + 0.5), 0, RENDERER_MAX_DISC_AREA_LIGHTS);
    if(ndotv>0.0 && rectAreaCount+discAreaCount>0) {
        // Prepare once; both area-light loops consume the same read-only surface.
        LtcSurface surface=PrepareLtcSurface(normal,viewDir,directSurface,roughness);
        for (int lightIndex = 0; lightIndex < rectAreaCount; ++lightIndex)
            directLight += EvaluateRectAreaLight(lighting.rectAreaLights[lightIndex],surface,albedo,worldPosition);
        for (int lightIndex = 0; lightIndex < discAreaCount; ++lightIndex)
            directLight += EvaluateDiscAreaLight(lighting.discAreaLights[lightIndex],surface,albedo,worldPosition);
    }
#endif

    return directLight;
}

#if RENDERER_DEPTH_PREPASS
[earlydepthstencil]
#endif
float4 main(PSInput input, bool frontFace : SV_IsFrontFace) : SV_TARGET
{
    uint textureFlags = drawTextureFlags;
    float4 base = baseColor * input.color;
    if ((textureFlags & RENDERER_TEXTURE_FLAG_BASE_COLOR) != 0u)
    {
        base *= SampleMaterial(0u,input.uv);
    }
    // 알파 모드 0은 불투명, 1은 마스크 잘라내기, 2는 블렌드다. 일반 경로에서 마스크 discard는 법선 미분 계산 뒤로 미뤄 픽셀 이웃 미분을 유지한다.
    uint alphaMode=padding1 & 3u;
    if((textureFlags&64u)!=0u) // LOD 표시용 비트면 텍스처 알파의 마스크만 유지하고 조명·텍스처 RGB를 쓰지 않은 표시색을 낸다.
    {
        if(alphaMode==1u) clip(base.a-asfloat(padding0)); // Mask 모드에서 cutoff보다 작은 픽셀을 버린다. 그 외 알파 모드는 이 잘라내기를 하지 않는다.
        float3 color=baseColor.rgb;
        if(lighting.pbrParams.z>.5) color=pow(color,1.0/2.2); // 이 출력 경로가 감마 적용을 요구할 때만 RGB에 1/2.2 거듭제곱을 적용한다.
        return float4(color,alphaMode==2u ? base.a : 1.0);
    }
    if((padding1&16u)!=0u) // unlit 재질이면 기본색만 출력 변환하고 일반 조명 계산을 건너뛴다.
    {
        if(alphaMode==1u)clip(base.a-asfloat(padding0)); // Mask 모드에서 cutoff보다 작은 픽셀을 버린다. 그 외 알파 모드는 이 잘라내기를 하지 않는다.
        float3 color=base.rgb;
        if(lighting.pbrParams.z>=0.0) // 직접 출력 경로만 노출과 톤 매핑을 적용한다. 음수는 선형 HDR로 남겨 후단 출력 패스에 맡긴다.
        {
            color=min(color*lighting.pbrParams.w,16777216.0);
            color=color/(color+1.0);
            if(lighting.pbrParams.z>.5)color=pow(color,1.0/2.2); // 이 출력 경로가 감마 적용을 요구할 때만 RGB에 1/2.2 거듭제곱을 적용한다.
        }
        color=float3(isnan(color.r)?0:clamp(color.r,0,65504),isnan(color.g)?0:clamp(color.g,0,65504),
            isnan(color.b)?0:clamp(color.b,0,65504));
        return float4(color,alphaMode==2u?base.a:1.0);
    }
    float3 albedo=base.rgb;

    float minRoughness = clamp(lighting.pbrParams.x, 0.01, 1.0);
    float ambientSpecularStrength = max(lighting.pbrParams.y, 0.0);
    float metallic = clamp(materialParams.x, 0.0, 1.0);
    float roughness = clamp(materialParams.y, minRoughness, 1.0);
    float occlusion = 1.0;
    // Opaque environment buffers already include authored AO and screen AO.
    bool usesEnvironmentBuffers=environmentSettings.x>0.5 && alphaMode!=2u;
    if ((textureFlags & RENDERER_TEXTURE_FLAG_METALLIC_ROUGHNESS) != 0u)
    {
        float4 metallicRoughness = SampleMaterial(1u,input.uv);
        roughness = clamp(roughness * metallicRoughness.g, minRoughness, 1.0);
        metallic = clamp(metallic * metallicRoughness.b, 0.0, 1.0);
    }
    if ((textureFlags & RENDERER_TEXTURE_FLAG_OCCLUSION) != 0u && !usesEnvironmentBuffers)
    {
        occlusion = lerp(1.0, SampleMaterial(3u,input.uv).r, clamp(materialParams.w, 0.0, 1.0));
    }
    float3 emissive = emissiveColor.rgb;
    if ((textureFlags & RENDERER_TEXTURE_FLAG_EMISSIVE) != 0u) // 발광 이미지가 있으면 발광 상수 RGB에 이미지를 곱한다.
    {
        emissive *= SampleMaterial(4u,input.uv).rgb;
    }

    float3 normal = GetNormal(input, textureFlags);
    // Derivatives precede discard and the two-sided normal orientation branch.
    float3 normalDx=ddx(normal),normalDy=ddy(normal);
    // 화면 이웃의 법선 변화량으로 좁은 반사의 흔들림을 줄인다. roughness^4에 제한된 분산을 더한 뒤 네제곱근으로 새 roughness를 만든다.
    float normalVariance=0.25*(dot(normalDx,normalDx)+dot(normalDy,normalDy));
    float filteredAlphaSquared=min(1.0,roughness*roughness*roughness*roughness+min(2.0*normalVariance,0.18));
    roughness=sqrt(sqrt(filteredAlphaSquared));
    if(alphaMode==1u) clip(base.a-asfloat(padding0)); // Mask 모드에서 cutoff보다 작은 픽셀을 버린다. 그 외 알파 모드는 이 잘라내기를 하지 않는다.
    if((padding1 & 4u)!=0u && !frontFace) normal=-normal; // 양면 재질의 뒷면이면 법선을 반대로 돌려 뒤쪽 표면 방향으로 조명한다.
    float3 viewDir = normalize(lighting.cameraPosition.xyz - input.worldPosition * lighting.cameraPosition.w);
    DirectSurface directSurface=PrepareDirectSurface(normal,viewDir,albedo,metallic,roughness);
    float3 f0=directSurface.f0;
    float3 directLight = EvaluateLights(lighting, input.worldPosition, normal, viewDir, albedo, roughness,directSurface);
    float3 ambientFresnel = FresnelSchlickRoughness(directSurface.ndotv, f0, roughness);
    float3 kD = (float3(1.0, 1.0, 1.0) - ambientFresnel) * (1.0 - metallic);
    float3 ambientDiffuse = kD * albedo * lighting.ambientColor.rgb * lighting.ambientColor.a;
    float3 ambientSpecular = ambientFresnel * lighting.environmentColor.rgb * lighting.environmentColor.a * ambientSpecularStrength;
    float3 ambient = (ambientDiffuse + ambientSpecular) * occlusion;
    float2 screenUV=input.position.xy*environmentScreen.xy;
    float screenAO=!usesEnvironmentBuffers && environmentScreen.z>0.5 ? ScreenAOTexture.SampleLevel(ScreenSampler,screenUV,0).r : 1.0;
    ambient*=screenAO;
    if(environmentSettings.x>0.5) // 환경광 패스가 활성화되면 기존 단색 주변광을 0으로 지우고, 알파 모드에 따라 SH 확산광 또는 화면 환경광 텍스처 결과로 다시 계산한다.
    {
        ambient=0;
        if(alphaMode==2u) // 투명 표면은 불투명 표면용 화면 환경광 대신 현재 법선의 SH 확산 경로로 간다.
        {
            if((uint(environmentScreen.w+0.5)&1u)!=0u) // 환경 diffuse 비트가 켜져 있을 때만 투명 표면의 SH 확산광을 더한다.
                ambient=kD*albedo*EnvironmentIrradiance(normal)*(0.3183098861837907*environmentSettings.y*occlusion*screenAO);
        }
        else
        {
            // 불투명 표면은 앞선 환경광 패스가 만든 화면 확산·반사 결과를 읽는다. 확산에는 현재 재질의 kD·albedo를 곱하고 반사는 그대로 더하며, 여기서 AO를 다시 곱하지 않는다.
            ambient=kD*albedo*EnvironmentDiffuseTexture.SampleLevel(ScreenSampler,screenUV,0).rgb;
            ambient+=EnvironmentSpecularTexture.SampleLevel(ScreenSampler,screenUV,0).rgb;
        }
    }
    float3 color = lighting.lightCounts.w > 0.5 ? ambient + directLight + emissive : albedo + emissive;

    if(lighting.pbrParams.z >= 0.0) { // 직접 출력 경로만 노출과 톤 매핑을 적용한다. 음수는 선형 HDR로 남겨 후단 출력 패스에 맡긴다.
        color = min(color * lighting.pbrParams.w, 16777216.0);
    color = color / (color + float3(1.0, 1.0, 1.0));
    if (lighting.pbrParams.z > 0.5) color = pow(color, float3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2)); // 이 출력 경로가 감마 적용을 요구할 때만 RGB에 1/2.2 거듭제곱을 적용한다.
    }
    // HDR 장면의 RGBA16F 저장 한도 65504로 RGB를 제한하고 NaN은 0으로 치환한다. 넘친 밝기가 후속 GI·bloom·시간 누적 통계까지 오염되는 일을 줄인다.
    color=float3(isnan(color.r)?0.0:clamp(color.r,0.0,65504.0),
        isnan(color.g)?0.0:clamp(color.g,0.0,65504.0),isnan(color.b)?0.0:clamp(color.b,0.0,65504.0));
    return float4(color, alphaMode==2u ? base.a : 1.0);
}
