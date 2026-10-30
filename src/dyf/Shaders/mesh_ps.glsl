#version 450
#extension GL_GOOGLE_include_directive : require

// 이 셰이더가 사용하는 바인딩과 입력 크기다.
#ifndef RENDERER_BINDLESS // 재질 텍스처 배열 방식이 지정되지 않았으면 개별 텍스처 바인딩을 기본으로 사용한다.
#define RENDERER_BINDLESS 0
#endif
#define RENDERER_DESCRIPTOR_SET 0
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

#define RENDERER_TEXTURE_FLAG_BASE_COLOR 1
#define RENDERER_TEXTURE_FLAG_METALLIC_ROUGHNESS 2
#define RENDERER_TEXTURE_FLAG_NORMAL 4
#define RENDERER_TEXTURE_FLAG_OCCLUSION 8
#define RENDERER_TEXTURE_FLAG_EMISSIVE 16
#define RENDERER_TEXTURE_FLAG_RECEIVE_SHADOW 32

#ifndef RENDERER_ENABLE_SHADOWS // 그림자 사용 여부를 파이프라인이 명시하지 않았으면 컴파일 오류를 내어 자원 배치 불일치를 막는다.
#error RENDERER_ENABLE_SHADOWS must be defined
#endif

// 표면에 수직인 방향(법선)과 표면을 따라가는 방향(접선)을 정점에서 받는다.
// UV·월드 위치·법선·접선을 픽셀 위치로 보간하고, gl_FrontFacing으로 받은 면 앞뒤 정보를 양면 재질에 반영한다.
layout(location = 0) in vec2 fragUv;
layout(location = 1) in vec3 fragWorldPosition;
layout(location = 2) in vec3 fragNormal;
layout(location = 3) in vec4 fragTangent;
#if RENDERER_ENABLE_SHADOWS // 그림자 빌드에서만 깊이 자원·시점 자료·가시율 계산을 포함한다. 꺼진 직접광 경로의 가시율은 1이다.
layout(location = 4) in vec4 fragLightSpacePosition;
#endif
layout(location = 0) out vec4 outColor;

#if !RENDERER_BINDLESS // 개별 바인딩 경로에서만 명명된 재질·화면 환경광 자원을 포함한다. bindless 경로는 재질 배열을 사용한다.
layout(set = RENDERER_DESCRIPTOR_SET, binding = RENDERER_BINDING_BASE_COLOR_TEXTURE) uniform texture2D baseColorTexture;
#endif
#if RENDERER_ENABLE_SHADOWS // 그림자 빌드에서만 깊이 자원·시점 자료·가시율 계산을 포함한다. 꺼진 직접광 경로의 가시율은 1이다.
layout(set = RENDERER_DESCRIPTOR_SET, binding = RENDERER_BINDING_SHADOW_TEXTURE) uniform texture2D shadowMap;
#endif
#if !RENDERER_BINDLESS // 개별 바인딩 경로에서만 명명된 재질·화면 환경광 자원을 포함한다. bindless 경로는 재질 배열을 사용한다.
layout(set = RENDERER_DESCRIPTOR_SET, binding = RENDERER_BINDING_METALLIC_ROUGHNESS_TEXTURE) uniform texture2D metallicRoughnessTexture;
layout(set = RENDERER_DESCRIPTOR_SET, binding = RENDERER_BINDING_NORMAL_TEXTURE) uniform texture2D normalTexture;
layout(set = RENDERER_DESCRIPTOR_SET, binding = RENDERER_BINDING_OCCLUSION_TEXTURE) uniform texture2D occlusionTexture;
layout(set = RENDERER_DESCRIPTOR_SET, binding = RENDERER_BINDING_EMISSIVE_TEXTURE) uniform texture2D emissiveTexture;
#endif
layout(set = RENDERER_DESCRIPTOR_SET, binding = RENDERER_BINDING_MATERIAL_SAMPLER) uniform sampler materialSampler;
// LTC 변환 표는 텍스처 26, 진폭 표는 27이다. 재질 샘플러 8로 표의 mip 0을 읽으며 64×64 표의 텍셀 중심에 맞춘 UV를 사용한다.
layout(set=0,binding=26) uniform texture2D ltcMatrixTexture;
layout(set=0,binding=27) uniform texture2D ltcAmplitudeTexture;
#if !RENDERER_BINDLESS // 개별 바인딩 경로에서만 명명된 재질·화면 환경광 자원을 포함한다. bindless 경로는 재질 배열을 사용한다.
layout(set=0,binding=16) uniform texture2D screenAOTexture;
layout(set=0,binding=13) uniform texture2D environmentSpecularTexture;
layout(set=0,binding=17) uniform texture2D environmentDiffuseTexture;
layout(set=0,binding=15) uniform sampler screenSampler;
// 개별 바인딩의 환경 상수는 연결 14에 있다. settings.x/y/z는 환경 사용·강도·회전이다.
// 주변 형상에 가려 주변광이 덜 닿는 정도를 나타내는 값(AO)의 사용 여부를 screen.z로 전달한다.
// screen.xy는 화면 역크기, w는 환경 기능 비트다. SH 9개와 probe 정보도 CPU와 같은 메모리 배치를 사용한다.
layout(set=0,binding=14) uniform EnvironmentConstants
{
    vec4 sh[9];
    vec4 settings;
    vec4 screen;
    vec4 probePosition;
    vec4 probeBoxMin;
    vec4 probeBoxMax;
} environment;
// 법선의 XY를 환경 회전만큼 돌리고 9개 구면조화 계수로 확산 입사광을 복원한다. 음수 결과는 버리며, 호출측이 albedo·1/pi·환경 강도를 곱한다.
vec3 EnvironmentIrradiance(vec3 n)
{
    float c=cos(environment.settings.z),s=sin(environment.settings.z);
    n.xy=mat2(c,-s,s,c)*n.xy;
    vec3 value=environment.sh[0].rgb*0.28209479177387814+
        environment.sh[1].rgb*(0.4886025119029199*n.y)+environment.sh[2].rgb*(0.4886025119029199*n.z)+
        environment.sh[3].rgb*(0.4886025119029199*n.x)+environment.sh[4].rgb*(1.0925484305920792*n.x*n.y)+
        environment.sh[5].rgb*(1.0925484305920792*n.y*n.z)+environment.sh[6].rgb*(0.31539156525252005*(3*n.z*n.z-1))+
        environment.sh[7].rgb*(1.0925484305920792*n.x*n.z)+environment.sh[8].rgb*(0.5462742152960396*(n.x*n.x-n.y*n.y));
    return max(value,vec3(0));
}
#endif
#if RENDERER_ENABLE_SHADOWS // 그림자 빌드에서만 깊이 자원·시점 자료·가시율 계산을 포함한다. 꺼진 직접광 경로의 가시율은 1이다.
layout(set = RENDERER_DESCRIPTOR_SET, binding = RENDERER_BINDING_SHADOW_SAMPLER) uniform sampler shadowSampler;
#endif

// CPU가 업로드하는 조명 버퍼의 필드 순서와 크기를 유지한다.
// 방향광은 표면에서 광원으로 향하는 방향·조도 강도·색을 가진다. 점/스폿 광원과 달리 거리 감쇠를 적용하지 않는다.
struct RendererDirectionalLight
{
	vec4 directionIntensity;
	vec4 color;
};

// CPU 공통 조명 버퍼의 점광원 위치·범위·색·강도 배치다. 현재 점/스폿 직접광 순회는 아래 ClusterLight 저장 버퍼를 사용한다.
struct RendererPointLight
{
	vec4 positionRange;
	vec4 colorIntensity;
};

// 스폿의 위치·거리 범위·방출 방향·내외 원뿔 계수를 공유 배치로 유지한다. 실제 군집 광원 계산에서는 같은 의미의 ClusterLight 필드를 읽는다.
struct RendererSpotLight
{
	vec4 positionRange;
	vec4 directionOuterCos;
	vec4 colorIntensity;
	vec4 coneParams;
};

// 사각 발광면 중심·강도, 방출 방향·전체 폭, up 축·전체 높이, RGB다. 이 사각형 경계를 LTC 적분에 넘긴다.
struct RendererRectAreaLight
{
	vec4 positionIntensity;
	vec4 directionWidth;
	vec4 upHeight;
	vec4 color;
};

// 원판 발광면의 중심·강도, 방향·반지름, up 축, RGB다. 원 경계를 16각형으로 근사해 LTC로 적분한다.
struct RendererDiscAreaLight
{
	vec4 positionIntensity;
	vec4 directionRadius;
	vec4 up;
	vec4 color;
};

// CPU가 올린 카메라·환경·조명·그림자 설정이다. 이전 호환 필드도 포함하므로 일부를 현재 계산이 쓰지 않더라도 구조체 필드 순서는 유지한다.
struct RendererLightingConstants
{
	vec4 cameraPosition;
	vec4 directionalLightDirection;
	vec4 directionalLightColor;
	vec4 ambientColor;
	vec4 shadowParams;
	vec4 pbrParams;
	vec4 environmentColor;
	vec4 pointLightPositionRange;
	vec4 pointLightColorIntensity;
	vec4 lightCounts;
	vec4 areaLightCounts;
	RendererDirectionalLight directionalLights[RENDERER_MAX_DIRECTIONAL_LIGHTS];
	RendererPointLight pointLights[RENDERER_MAX_POINT_LIGHTS];
	RendererSpotLight spotLights[RENDERER_MAX_SPOT_LIGHTS];
	RendererRectAreaLight rectAreaLights[RENDERER_MAX_RECT_AREA_LIGHTS];
	RendererDiscAreaLight discAreaLights[RENDERER_MAX_DISC_AREA_LIGHTS];
	vec4 shadowLight;
};
layout(set = RENDERER_DESCRIPTOR_SET, binding = RENDERER_BINDING_LIGHTING_CONSTANTS) uniform RendererLighting { RendererLightingConstants lighting; };
// 국소 광원의 위치/거리, RGB/강도, 스폿 방향/외곽 cos, 내부 cos·광원 종류·그림자 번호를 한 항목으로 묶는다.
// 연결 19의 광원 배열, 20의 군집 목록, 21의 군집 투영 상수를 함께 사용한다. GLSL은 set 0, HLSL은 t19/t20/b21, Metal은 buffer 19/20/21이다.
struct ClusterLight {vec4 positionRange;vec4 colorIntensity;vec4 directionOuterCos;vec4 coneShadow;};
layout(std430,set=0,binding=19) readonly buffer ClusterLights {ClusterLight clusterLights[];};
layout(std430,set=0,binding=20) readonly buffer ClusterLists {uint clusterList[];};
layout(set=0,binding=21) uniform ClusterParameters {mat4 view;mat4 projection;mat4 inverseProjection;vec4 depth;uvec4 dimensions;uvec4 counts;} clusters;
// 월드 위치를 군집 카메라로 투영하여 화면 XY와 깊이 슬라이스를 구한다. 원근 카메라는 로그 깊이, 직교 카메라는 선형 깊이를 쓴다.
// 반환값은 개수 1칸과 광원 인덱스 목록이 이어진 군집 구간의 시작이다. 0xffffffff는 목록을 사용할 수 없다는 표시이며 전체 국소 광원 순회로 대체한다.
uint ClusterBase(vec3 position)
{
    if(clusters.counts.y==0u)return 0xffffffffu; // 군집 목록 사용이 꺼져 있으면 전체 국소 광원 순회 표식을 반환한다.
    vec4 viewPosition=clusters.view*vec4(position,1),clip=clusters.projection*viewPosition;
    float distance=-viewPosition.z;
    if(distance<clusters.depth.x || distance>clusters.depth.y || clip.w<=0)return 0xffffffffu; // 군집 깊이 범위 밖이거나 투영 뒤쪽이면 유효한 군집을 고를 수 없다.
    vec2 coordinate=clip.xy/clip.w*.5+.5;
    if(any(lessThan(coordinate,vec2(0))) || any(greaterThanEqual(coordinate,vec2(1))))return 0xffffffffu; // 군집 화면 범위 [0,1) 밖이면 전체 국소 광원 경로로 돌린다.
    float slice=clusters.counts.z!=0u?log(distance/clusters.depth.x)/clusters.depth.z:
        (distance-clusters.depth.x)/(clusters.depth.y-clusters.depth.x);
    uvec3 index=min(uvec3(coordinate*vec2(clusters.dimensions.xy),slice*float(clusters.dimensions.z)),clusters.dimensions.xyz-uvec3(1));
    return (index.x+clusters.dimensions.x*(index.y+clusters.dimensions.y*index.z))*(clusters.dimensions.w+1u);
}

// 한 draw의 공통 행렬과 재질 입력이다. 정점 단계와 192바이트 배치를 공유하며 draw 상수는 GLSL push constant, HLSL b10, Metal buffer 10으로 전달된다.
// padding0은 알파 cutoff의 float 비트, padding1 하위 2비트는 Opaque/Mask/Blend, 4는 양면, 8은 법선 Y 반전, 16은 unlit이다. padding2는 bindless 재질 표 번호다.
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

// 거칠기의 제곱을 미세면 분포 폭으로 쓰는 GGX 분포다. 두 벡터 모두에 수직인 벡터를 구하는 연산(외적)을 법선과 반각 벡터에 적용한다.
// 날카로운 반사에서 1-(N·H)^2를 직접 빼면 유효 자릿수를 잃기 쉬우므로 같은 값을 외적 결과의 길이 제곱으로 계산한다.
float DistributionGGX(vec3 normal, vec3 halfway, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float ndoth = clamp(dot(normal, halfway), 0.0, 1.0);
    vec3 crossNH = cross(normal, halfway);
    float denom = dot(crossNH, crossNH) + ndoth * ndoth * a2;
    return a2 / (PI * denom * denom);
}

// 시선이나 광원이 비스듬할 때 미세 표면끼리 가리는 정도를 근사한다. 직접광에서 시선 항과 광원 항을 곱해 사용한다.
float GeometrySchlickGGX(float ndotv, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return ndotv / max(ndotv * (1.0 - k) + k, 0.0001);
}

// 정면 반사율 f0에서 시작해 비스듬히 볼수록 반사가 커지는 Fresnel 효과를 5제곱으로 근사한다.
vec3 FresnelSchlick(float cosTheta, vec3 f0) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// 주변광과 면광원 확산 가중치용 Fresnel 근사다. 거친 표면에서 가장자리 반사가 과해지지 않도록 roughness로 상한을 조절한다.
vec3 FresnelSchlickRoughness(float cosTheta, vec3 f0, float roughness) {
    vec3 roughnessF0 = max(vec3(1.0 - roughness), f0);
    return f0 + (roughnessF0 - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

#if RENDERER_BINDLESS // bindless 빌드이면 재질 배열·인덱스 표와 그에 맞는 그림자 연결 번호를 사용한다.
layout(set=0,binding=0) uniform texture2D materialTextures[19];
layout(std430,set=0,binding=29) readonly buffer MaterialTextureIndices {uvec4 materialTextureIndices[];};
#endif
// kind 0~4는 기본색·금속/거칠기·법선·AO·발광이다. bindless 경로는 재질당 uint4 두 칸 중 kind/4번 칸의 kind%4 성분을 읽어 19개 텍스처 배열의 인덱스로 사용한다.
// 개별 경로는 0/4/5/6/7번 텍스처와 8번 샘플러를 사용한다. 텍스처의 sRGB/Linear 해석은 연결된 텍스처 형식이 담당한다.
vec4 SampleMaterial(uint kind,vec2 uv)
{
#if RENDERER_BINDLESS // bindless 빌드이면 재질 배열·인덱스 표와 그에 맞는 그림자 연결 번호를 사용한다.
    uint index=materialTextureIndices[pushConstants.padding2*2u+kind/4u][kind%4u];
    return texture(sampler2D(materialTextures[index],materialSampler),uv);
#else
    if(kind==0u) return texture(sampler2D(baseColorTexture,materialSampler),uv); // 기본색 종류는 0번 텍스처를 읽는다.
    if(kind==1u) return texture(sampler2D(metallicRoughnessTexture,materialSampler),uv); // 금속/거칠기 종류는 4번 텍스처를 읽는다.
    if(kind==2u) return texture(sampler2D(normalTexture,materialSampler),uv); // 법선 종류는 5번 텍스처를 읽는다.
    if(kind==3u) return texture(sampler2D(occlusionTexture,materialSampler),uv); // AO 종류는 6번 텍스처를 읽고, 남은 발광 종류는 아래 7번 텍스처로 간다.
    return texture(sampler2D(emissiveTexture,materialSampler),uv);
#endif
}

// 보간 법선을 정규화하고 접선을 법선에 직교화해 TBN 기준축을 만든다. 법선 맵을 -1~1로 바꾸고 XY 강도·선택적 Y 반전을 적용해 월드 법선으로 옮긴다.
vec3 GetNormal() {
    vec3 normal = normalize(fragNormal);
    int textureFlags = int(pushConstants.textureFlags);
    float normalScale = pushConstants.materialParams.z;
    if ((textureFlags & RENDERER_TEXTURE_FLAG_NORMAL) == 0 || abs(normalScale) <= 0.0001) { // 법선 이미지가 없거나 법선 강도가 사실상 0이면 보간된 월드 법선을 그대로 사용한다.
        return normal;
    }

    vec3 tangent = normalize(fragTangent.xyz - normal * dot(normal, fragTangent.xyz));
    vec3 bitangent = normalize(cross(normal, tangent)) * fragTangent.w;
    // GLSL·Metal은 T/B/N을 열로 놓고 행렬×법선을 계산한다. HLSL은 생성자의 행으로 놓은 뒤 법선×행렬을 계산하여 같은 월드 방향을 얻는다.
    mat3 tbn = mat3(tangent, bitangent, normal);
    vec3 tangentNormal = SampleMaterial(2u,fragUv).xyz * 2.0 - 1.0;
    tangentNormal.xy *= normalScale;
    if((pushConstants.padding1 & 8u)!=0u) tangentNormal.y=-tangentNormal.y; // 법선 맵의 Y 방향이 반대인 재질이면 접선 공간 Y 성분만 뒤집는다.
    return normalize(tbn * tangentNormal);
}

#if RENDERER_ENABLE_SHADOWS // 그림자 빌드에서만 깊이 자원·시점 자료·가시율 계산을 포함한다. 꺼진 직접광 경로의 가시율은 1이다.
// 광원별 시작 시점·개수·강도·광원 크기, 캐스케이드 경계, 아틀라스 영역과 PCSS용 투영 계수를 공유한다. 그림자 좌표는 선택한 시점 행렬로 월드 위치를 다시 투영해 계산한다.
layout(set=0,binding=RENDERER_BINDING_SHADOW_MATRIX) uniform ShadowMatrix {
    mat4 lightViewProjectionMatrix[128];
    vec4 atlasRect[128];
    vec4 directionalViews[RENDERER_MAX_DIRECTIONAL_LIGHTS];
    vec4 pointViews[RENDERER_MAX_POINT_LIGHTS];
    vec4 spotViews[RENDERER_MAX_SPOT_LIGHTS];
    vec4 directionalSplits[RENDERER_MAX_DIRECTIONAL_LIGHTS];
    mat4 cameraView;
    vec4 filterParams;
    vec4 viewProjectionParams[128];
    vec4 softShadowParams;
} shadowMatrix;

#define DY_SHADOW_PROJECTION(index) shadowMatrix.viewProjectionParams[index]
#define DY_SHADOW_MATRIX(index) shadowMatrix.lightViewProjectionMatrix[index]
#define DY_SHADOW_TILE(index) shadowMatrix.atlasRect[index]
#define DY_SHADOW_DIMENSIONS textureSize(sampler2D(shadowMap,shadowSampler),0)
#define DY_SHADOW_READ(pixel) texelFetch(sampler2D(shadowMap,shadowSampler),pixel,0).r
#define DY_SHADOW_FILTER shadowMatrix.filterParams
#define DY_SHADOW_SOFT shadowMatrix.softShadowParams
// 현재 자원 읽기 함수를 DY_SHADOW 매크로로 연결한 뒤 공통 PCSS 구현을 포함한다. 같은 필터 알고리즘을 언어별 타입과 바인딩에 맞춰 쓴다.
#include "shadow_filter.glsl"

// 한 그림자 시점의 가시율을 구한다. 0~1 깊이와 위아래를 뒤집은 UV를 아틀라스 영역에 넣고, 같은 표면이 자기 그림자로 판정되는 오차를 줄이도록 표면 기울기 기반 bias를 수신 깊이에서 뺀다.
// 광원 크기가 있으면 PCSS로 반그림자를, 없으면 고정 텍셀 반경 PCF로 가시 표본 비율을 구한다. strength는 최종 어두워지는 양만 조절한다.
float SampleShadowView(int index,vec3 worldPosition,vec3 normal,vec3 lightDir,float strength,float sourceRadius,int pointBase)
{
    vec4 clip=shadowMatrix.lightViewProjectionMatrix[index]*vec4(worldPosition,1.0);
    if(clip.w<=0.0)return 1.0; // 선택한 광원 시점 뒤쪽이면 해당 시점 그림자를 적용하지 않는다.
    vec3 ndc=clip.xyz/clip.w;
    vec2 uv=ndc.xy*.5+.5;
    uv.y=1.0-uv.y;
    if(uv.x<0.0||uv.y<0.0||uv.x>1.0||uv.y>1.0||ndc.z<0.0||ndc.z>1.0)return 1.0; // 그림자 시점의 UV 또는 0~1 깊이 범위 밖이면 완전 가시로 둔다.
    vec4 tile=shadowMatrix.atlasRect[index];
    vec2 texel=1.0/vec2(textureSize(sampler2D(shadowMap,shadowSampler),0));
    vec2 minUv=tile.xy+texel*.5;
    vec2 maxUv=tile.xy+tile.zw-texel*.5;
    vec2 localUv=uv;
    uv=tile.xy+uv*tile.zw;
    float angle=1.0-max(dot(normal,lightDir),0.0);
    float bias=max(lighting.shadowParams.x,lighting.shadowParams.y*angle)+lighting.shadowParams.z*angle;
    float receiver=ndc.z-bias;
    if(sourceRadius>0 && shadowMatrix.softShadowParams.x>0) // 광원 크기와 허용 소프트 반경이 모두 양수일 때만 PCSS를 사용한다.
        return 1-clamp(strength,0.0,1.0)*(1-DyShadowPcss(index,pointBase,localUv,receiver,clip.w,lightDir,sourceRadius));
    float radius=min(lighting.shadowParams.w,shadowMatrix.filterParams.z);
    int extent=int(ceil(radius));
    float visible=0.0,samples=float((2*extent+1)*(2*extent+1));
    for(int y=-extent;y<=extent;++y)for(int x=-extent;x<=extent;++x)
    {
        vec2 sampleUv=clamp(uv+vec2(x,y)*texel,minUv,maxUv);
        visible+=receiver<=texture(sampler2D(shadowMap,shadowSampler),sampleUv).r?1.0:0.0;
    }
    return 1.0-clamp(strength,0.0,1.0)*(1.0-visible/max(samples,1.0));
}

// 방향광은 카메라 깊이의 캐스케이드, 점광원은 광원→표면 방향의 큐브면, 스폿은 해당 시점을 고른다. 방향광 경계 마지막 10%에서는 다음 캐스케이드와 섞는다.
float SampleShadow(int type,int lightIndex,vec3 worldPosition,vec3 normal,vec3 lightDir)
{
    if((pushConstants.textureFlags & RENDERER_TEXTURE_FLAG_RECEIVE_SHADOW)==0u)return 1.0; // 객체의 그림자 받기 비트가 꺼져 있으면 가시율 1로 조명을 그대로 받는다.
    vec4 views=type==1?shadowMatrix.directionalViews[lightIndex]:(type==2?shadowMatrix.pointViews[lightIndex]:shadowMatrix.spotViews[lightIndex]);
    int count=int(views.y+.5);
    if(count==0)return 1.0; // 광원에 배정된 그림자 시점이 없으면 가시율 1을 반환한다.
    int local=0;
    float depth=0.0;
    if(type==1) // 방향광이면 카메라 앞쪽 거리로 캐스케이드를 선택한다.
    {
        depth=-(shadowMatrix.cameraView*vec4(worldPosition,1.0)).z;
        float finalSplit=shadowMatrix.directionalSplits[lightIndex][count-1];
        if(finalSplit>0 && depth>finalSplit)return 1.0; // 마지막 유효 캐스케이드 거리보다 멀면 그림자를 적용하지 않는다.
        while(local<count-1 && depth>shadowMatrix.directionalSplits[lightIndex][local])++local;
    }
    if(type==2) // 점광원이면 광원에서 표면으로 향하는 방향의 주요 축으로 큐브면을 고른다.
    {
        vec3 direction=-lightDir, magnitude=abs(direction);
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
        float blend=smoothstep(mix(previous,split,.9),split,depth);
        if(blend>0.0)visibility=mix(visibility,SampleShadowView(index+1,worldPosition,normal,lightDir,views.z,views.w,type==2?int(views.x+.5):-1),blend); // 캐스케이드 경계의 혼합 구간에서만 다음 시점 그림자도 읽는다.
    }
    return visibility;
}

#endif

// 여기서 구한 거리 감쇠는 아래 국소 광원 루프에서 광원 RGB·강도에 곱해 EvaluateDirectLight의 입사 조도로 전달한다.
// 거리 제곱 역수에 거리 범위의 끝에서 0으로 부드럽게 닫히는 창 함수를 곱한다. 광원과 아주 가까운 거리의 분모는 작은 양수로 제한한다.
float PunctualAttenuation(float distanceToLight, float range) {
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
float SpotConeAttenuation(float cosTheta, float innerCos, float outerCos) {
    float denominator = max(innerCos - outerCos, 0.0001);
    float t = clamp((cosTheta - outerCos) / denominator, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// 한 광원에 대한 GGX 반사와 에너지 가중 Lambert 확산을 더한다. 입사 조도·ndotl·그림자 가시율을 곱하며, ndotl은 max(dot(normal, lightDir), 0)이다. 금속도가 높을수록 확산 비중은 줄어든다.
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
    if (ndotl <= 0.0 || ndotv <= 0.0 || visibility <= 0.0) { // 광원 또는 시선이 법선의 반대쪽이나 표면과 나란한 방향에 있거나, 가시율이 0 이하면 이 광원의 기여 RGB를 0으로 반환한다.
        return vec3(0.0,0.0,0.0);
    }
    vec3 halfwayVector = viewDir + lightDir;
    float halfwayLengthSquared = dot(halfwayVector, halfwayVector);
    if (halfwayLengthSquared <= 0.000001) { // 시선과 광원 방향이 상쇄되어 반각 벡터를 정규화할 수 없으면 0을 반환한다.
        return vec3(0.0,0.0,0.0);
    }
    vec3 halfway = halfwayVector * inversesqrt(halfwayLengthSquared);
    vec3 f0 = mix(vec3(0.04,0.04,0.04), albedo, metallic);
    float ndf = DistributionGGX(normal, halfway, roughness);
    float geometry = GeometrySchlickGGX(ndotv, roughness) * GeometrySchlickGGX(ndotl, roughness);
    vec3 fresnel = FresnelSchlick(max(dot(halfway, viewDir), 0.0), f0);
    vec3 specular = (ndf * geometry * fresnel) / max(4.0 * ndotv * ndotl, 0.0001);
    vec3 diffuseWeight = (vec3(1.0,1.0,1.0) - fresnel) * (1.0 - metallic);
    return (diffuseWeight * albedo / PI + specular) * incidentIlluminance * ndotl * visibility;
}

// LTC 적분과 표 배치는 Heitz 외 SIGGRAPH 2016 및 Assets/ltc/LICENSE를 따른다.
// 정규화된 두 경계 방향 사이의 구면 모서리 적분을 계산한다. 모서리들을 순서대로 더해 면광원이 차지하는 cosine 가중 영역을 얻는다.
float LtcEdgeIntegral(vec3 a,vec3 b)
{
    vec3 edge=cross(a,b);
    float sine=length(edge),cosine=clamp(dot(a,b),-1.0,1.0);
    return edge.z*(atan(sine,cosine)/max(sine,1e-12))*0.15915494309189535;
}
// 광원 꼭짓점의 상대 위치를 표면 접선 기준축으로 옮긴 뒤 LTC 역행렬을 적용한다. 표 조회 결과의 4성분만으로 필요한 변환을 구성한다.
vec3 LtcTransform(vec3 offset,vec3 tangent,vec3 bitangent,vec3 normal,vec4 inverseMatrix)
{
    vec3 local=vec3(dot(offset,tangent),dot(offset,bitangent),dot(offset,normal));
    return vec3(inverseMatrix.x*local.x+inverseMatrix.z*local.z,local.y,
        inverseMatrix.y*local.x+inverseMatrix.w*local.z);
}
// 면광원 경계를 LTC 공간으로 옮기고 z>=0인 표면 위쪽 반구에 자른다. 잘린 점을 정규화하며 바로 모서리 적분에 더하므로 별도 잘린 다각형 배열이 필요 없다.
float LtcPolygonIntegral(vec3 normal,vec3 viewDir,vec3 position,
    vec4 inverseMatrix,vec3 points[16],int count)
{
    vec3 tangent=viewDir-normal*dot(normal,viewDir);
    if(dot(tangent,tangent)<1e-8) // 시선의 접선 성분이 너무 작으면 법선과 평행하지 않은 축으로 접선을 만든다.
        tangent=cross(abs(normal.z)<.999?vec3(0,0,1):vec3(0,1,0),normal);
    tangent=normalize(tangent);
    vec3 bitangent=cross(normal,tangent);
    vec3 previous=LtcTransform(points[count-1]-position,tangent,bitangent,normal,inverseMatrix);
    vec3 first=vec3(0),last=vec3(0);bool haveVertex=false;float sum=0;
    for(int edge=0;edge<count;++edge)
    {
        vec3 current=LtcTransform(points[edge]-position,tangent,bitangent,normal,inverseMatrix);
        bool previousInside=previous.z>=0,currentInside=current.z>=0;
        for(int candidate=0;candidate<2;++candidate)
        {
            vec3 point;
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
            point*=inversesqrt(squared);
            if(haveVertex)sum+=LtcEdgeIntegral(last,point);else {first=point;haveVertex=true;} // 첫 점은 시작점으로 저장하고 이후 점은 직전 점과의 모서리 적분을 더한다.
            last=point;
        }
        previous=current;
    }
    if(haveVertex)sum+=LtcEdgeIntegral(last,first); // 점이 하나라도 있으면 마지막 점에서 첫 점으로 경계를 닫는다.
    return clamp(abs(sum),0.0,1.0);
}
// 거칠기와 N·V로 64×64 LTC 표의 변환·진폭을 읽는다. 반사 적분과 단위행렬의 확산 적분을 별도로 구해 금속도·Fresnel 가중치와 발광면 radiance를 곱한다.
vec3 EvaluateLtcPolygon(vec3 normal,vec3 viewDir,vec3 position,vec3 points[16],int count,
    vec3 albedo,float metallic,float roughness,vec3 radiance)
{
    float ndotv=clamp(dot(normal,viewDir),0.0,1.0);
    if(ndotv<=0)return vec3(0); // 시선이 표면 뒤쪽이면 이 면광원 BRDF 기여는 0이다.
    vec2 coordinate=(vec2(clamp(roughness,0.0,1.0),sqrt(1-ndotv))*63.0+.5)/64.0;
    vec4 inverseMatrix=textureLod(sampler2D(ltcMatrixTexture,materialSampler),coordinate,0);
    vec4 amplitude=textureLod(sampler2D(ltcAmplitudeTexture,materialSampler),coordinate,0);
    float specular=LtcPolygonIntegral(normal,viewDir,position,inverseMatrix,points,count);
    float diffuse=LtcPolygonIntegral(normal,viewDir,position,vec4(1,0,0,1),points,count);
    vec3 f0=mix(vec3(.04),albedo,metallic);
    vec3 specularWeight=f0*max(amplitude.x,0.0)+(vec3(1)-f0)*max(amplitude.y,0.0);
    vec3 diffuseWeight=(vec3(1)-FresnelSchlickRoughness(ndotv,f0,roughness))*(1-metallic);
    // 정규화된 cosine 적분이므로 diffuse에 1/pi나 거리 감쇠를 다시 곱하지 않는다.
    return radiance*(specularWeight*specular+diffuseWeight*albedo*diffuse);
}
// 방출 방향과 up으로 사각형 축을 만들고 네 모서리를 적분한다. 폭·높이가 없거나 발광면 뒤에 있는 표면은 빛을 받지 않는다.
vec3 EvaluateRectAreaLight(RendererRectAreaLight light,vec3 normal,vec3 viewDir,
    vec3 albedo,float metallic,float roughness,vec3 worldPosition)
{
    vec3 direction=normalize(light.directionWidth.xyz);
    if(light.directionWidth.w<=0 || light.upHeight.w<=0 || dot(direction,worldPosition-light.positionIntensity.xyz)<=0) // 면적이 없거나 사각 광원의 뒷면에 있는 수신점은 조명하지 않는다.
        return vec3(0);
    vec3 right=normalize(cross(direction,light.upHeight.xyz));
    vec3 up=normalize(cross(right,direction));
    vec3 x=right*(light.directionWidth.w*.5),y=up*(light.upHeight.w*.5),center=light.positionIntensity.xyz;
    vec3 points[16];
    points[0]=center-x-y;points[1]=center+x-y;points[2]=center+x+y;points[3]=center-x+y;
    return EvaluateLtcPolygon(normal,viewDir,worldPosition,points,4,albedo,metallic,roughness,
        light.color.rgb*max(light.positionIntensity.w,0.0));
}
// 원판 경계를 같은 각도 간격의 16각형으로 근사해 LTC로 면 전체를 적분하며, 점광원 16개의 합이나 거리 감쇠 표본 방식으로 계산하지 않는다.
vec3 EvaluateDiscAreaLight(RendererDiscAreaLight light,vec3 normal,vec3 viewDir,
    vec3 albedo,float metallic,float roughness,vec3 worldPosition)
{
    vec3 direction=normalize(light.directionRadius.xyz);
    if(light.directionRadius.w<=0 || dot(direction,worldPosition-light.positionIntensity.xyz)<=0)return vec3(0); // 반지름이 없거나 원판 광원의 뒷면에 있는 수신점은 조명하지 않는다.
    vec3 right=normalize(cross(direction,light.up.xyz));
    vec3 up=normalize(cross(right,direction));
    vec3 points[16];
    for(int corner=0;corner<16;++corner)
    {
        float angle=float(corner)*0.39269908169872415;
        points[corner]=light.positionIntensity.xyz+(right*cos(angle)+up*sin(angle))*light.directionRadius.w;
    }
    return EvaluateLtcPolygon(normal,viewDir,worldPosition,points,16,albedo,metallic,roughness,
        light.color.rgb*max(light.positionIntensity.w,0.0));
}

// 방향광 → 군집의 점/스폿 광원 → 사각·원판 면광원 순서로 직접광을 누적한다. 군집을 쓸 수 없거나 목록이 overflow 표시면 전체 국소 광원을 검사한다.
vec3 EvaluateLights(RendererLightingConstants lighting, vec3 worldPosition, vec3 normal, vec3 viewDir,
    vec3 albedo, float metallic, float roughness) {
    vec3 directLight = vec3(0.0,0.0,0.0);
    int directionalCount = clamp(int(lighting.lightCounts.x + 0.5), 0, RENDERER_MAX_DIRECTIONAL_LIGHTS);
    for (int lightIndex = 0; lightIndex < directionalCount; ++lightIndex) {
        RendererDirectionalLight light = lighting.directionalLights[lightIndex];
        vec3 lightDir = normalize(light.directionIntensity.xyz);
#if RENDERER_ENABLE_SHADOWS // 그림자 빌드에서만 깊이 자원·시점 자료·가시율 계산을 포함한다. 꺼진 직접광 경로의 가시율은 1이다.
        float visibility = SampleShadow(1,lightIndex,worldPosition,normal,lightDir);
#else
        float visibility = 1.0;
#endif
        directLight += EvaluateDirectLight(normal, viewDir, lightDir,
            light.color.rgb * max(light.directionIntensity.w, 0.0), albedo, metallic, roughness, visibility);
    }
    uint base=ClusterBase(worldPosition);
    uint listed=0xffffffffu;
    if(base!=0xffffffffu)listed=clusterList[base]; // 유효한 군집 주소일 때만 목록 개수를 읽는다. 무효 주소를 버퍼 인덱스로 쓰지 않는다.
    // 목록의 첫 칸이 0xffffffff이면 누락 없이 전체 clusterLights를 순회한다. 정상 목록은 해당 군집에 들어간 인덱스만 순회한다.
    uint localCount=listed==0xffffffffu?clusters.counts.x:listed;
    for(uint item=0u;item<localCount;++item)
    {
        uint lightIndex=listed==0xffffffffu?item:clusterList[base+1u+item];
        ClusterLight light=clusterLights[lightIndex];
        vec3 toLight=light.positionRange.xyz-worldPosition;
        float distanceToLight=length(toLight);
        if(distanceToLight<=0.0001 || distanceToLight>=light.positionRange.w)continue; // 광원과 거의 같은 위치이거나 거리 범위 밖인 국소 광원은 건너뛴다.
        vec3 lightDir=toLight/distanceToLight;
        float attenuation=PunctualAttenuation(distanceToLight,light.positionRange.w);
        int type=int(light.coneShadow.y+0.5),shadowIndex=int(light.coneShadow.z);
        if(type==3)attenuation*=SpotConeAttenuation(dot(-lightDir,light.directionOuterCos.xyz), // 스폿 광원은 거리 감쇠에 원뿔 각도 감쇠를 추가한다.
            light.coneShadow.x,light.directionOuterCos.w);
#if RENDERER_ENABLE_SHADOWS // 그림자 빌드에서만 깊이 자원·시점 자료·가시율 계산을 포함한다. 꺼진 직접광 경로의 가시율은 1이다.
        float visibility=shadowIndex>=0?SampleShadow(type,shadowIndex,worldPosition,normal,lightDir):1.0;
#else
        float visibility=1.0;
#endif
        vec3 incidentIlluminance=light.colorIntensity.rgb*max(light.colorIntensity.w,0.0)*attenuation;
        directLight+=EvaluateDirectLight(normal,viewDir,lightDir,incidentIlluminance,albedo,metallic,roughness,visibility);
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

// 기본색·알파를 먼저 읽고 LOD 표시/unlit를 분리한다. 일반 경로는 재질과 법선 → 직접광·주변광·발광 → 출력 변환 순서로 RGBA를 만든다.
void main() {
    int textureFlags = int(pushConstants.textureFlags);
    vec4 base = pushConstants.baseColor;
    if ((textureFlags & RENDERER_TEXTURE_FLAG_BASE_COLOR) != 0) { // 기본색 이미지가 있으면 재질 기본색 RGBA에 이미지 RGBA를 곱한다.
        base *= SampleMaterial(0u,fragUv);
    }
    // 알파 모드 0은 불투명, 1은 마스크 잘라내기, 2는 블렌드다. 일반 경로에서 마스크 discard는 법선 미분 계산 뒤로 미뤄 픽셀 이웃 미분을 유지한다.
    uint alphaMode=pushConstants.padding1 & 3u;
    if((pushConstants.textureFlags&64u)!=0u) // 렌더러가 관리하는 LOD 표시 비트(6)는 텍스처 알파 마스크만 유지하고 텍스처 RGB·조명을 제외한 표시색을 낸다.
    {
        if(alphaMode==1u && base.a<uintBitsToFloat(pushConstants.padding0)) discard; // Mask 모드에서 cutoff보다 작은 픽셀을 버린다. 그 외 알파 모드는 이 잘라내기를 하지 않는다.
        vec3 color=pushConstants.baseColor.rgb;
        if(lighting.pbrParams.z>.5) color=pow(color,vec3(1.0/2.2)); // 이 출력 경로가 감마 적용을 요구할 때만 RGB에 1/2.2 거듭제곱을 적용한다.
        outColor=vec4(color,alphaMode==2u ? base.a : 1.0);return;
    }
    if((pushConstants.padding1&16u)!=0u) // unlit 재질이면 기본색만 출력 변환하고 일반 조명 계산을 건너뛴다.
    {
        if(alphaMode==1u && base.a<uintBitsToFloat(pushConstants.padding0)) discard; // Mask 모드에서 cutoff보다 작은 픽셀을 버린다. 그 외 알파 모드는 이 잘라내기를 하지 않는다.
        vec3 color=base.rgb;
        if(lighting.pbrParams.z>=0.0) // 직접 출력 경로만 노출과 톤 매핑을 적용한다. 음수는 선형 HDR로 남겨 후단 출력 패스에 맡긴다.
        {
            color=min(color*lighting.pbrParams.w,vec3(16777216.0));
            color=color/(color+vec3(1));
            if(lighting.pbrParams.z>.5)color=pow(color,vec3(1.0/2.2)); // 이 출력 경로가 감마 적용을 요구할 때만 RGB에 1/2.2 거듭제곱을 적용한다.
        }
        color=vec3(isnan(color.r)?0:clamp(color.r,0,65504),isnan(color.g)?0:clamp(color.g,0,65504),
            isnan(color.b)?0:clamp(color.b,0,65504));
        outColor=vec4(color,alphaMode==2u?base.a:1.0);return;
    }
    vec3 albedo=base.rgb;

    float minRoughness = clamp(lighting.pbrParams.x, 0.01, 1.0);
    float ambientSpecularStrength = max(lighting.pbrParams.y, 0.0);
    float metallic = clamp(pushConstants.materialParams.x, 0.0, 1.0);
    float roughness = clamp(pushConstants.materialParams.y, minRoughness, 1.0);
    float occlusion = 1.0;
    if ((textureFlags & RENDERER_TEXTURE_FLAG_METALLIC_ROUGHNESS) != 0) { // 이미지 G로 거칠기, B로 금속도를 곱하고 유효 범위로 제한한다.
        vec4 metallicRoughness = SampleMaterial(1u,fragUv);
        roughness = clamp(roughness * metallicRoughness.g, minRoughness, 1.0);
        metallic = clamp(metallic * metallicRoughness.b, 0.0, 1.0);
    }
    if ((textureFlags & RENDERER_TEXTURE_FLAG_OCCLUSION) != 0) { // 이미지 R의 AO를 재질 강도와 섞어 주변광 차폐량을 만든다.
        occlusion = mix(1.0, SampleMaterial(3u,fragUv).r, clamp(pushConstants.materialParams.w, 0.0, 1.0));
    }

    vec3 normal = GetNormal();
    // Pixel-normal footprint filtering matches Surface and transparent shading.
    // Evaluate derivatives before alpha discard and per-face orientation changes.
    vec3 normalDx=dFdx(normal),normalDy=dFdy(normal);
    // 화면 이웃의 법선 변화량으로 좁은 반사의 흔들림을 줄인다. roughness^4에 제한된 분산을 더한 뒤 네제곱근으로 새 roughness를 만든다.
    float normalVariance=0.25*(dot(normalDx,normalDx)+dot(normalDy,normalDy));
    float filteredAlphaSquared=min(1.0,roughness*roughness*roughness*roughness+min(2.0*normalVariance,0.18));
    roughness=sqrt(sqrt(filteredAlphaSquared));
    if(alphaMode==1u && base.a<uintBitsToFloat(pushConstants.padding0)) discard; // Mask 모드에서 cutoff보다 작은 픽셀을 버린다. 그 외 알파 모드는 이 잘라내기를 하지 않는다.
    if((pushConstants.padding1 & 4u)!=0u && !gl_FrontFacing) normal=-normal; // 양면 재질의 뒷면이면 법선을 반대로 돌려 뒤쪽 표면 방향으로 조명한다.
    vec3 viewDir = normalize(lighting.cameraPosition.xyz - fragWorldPosition);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 directLight = EvaluateLights(lighting, fragWorldPosition, normal, viewDir, albedo, metallic, roughness);
    vec3 ambientFresnel = FresnelSchlickRoughness(max(dot(normal, viewDir), 0.0), f0, roughness);
    vec3 kD = (vec3(1.0) - ambientFresnel) * (1.0 - metallic);
    vec3 ambientDiffuse = kD * albedo * lighting.ambientColor.rgb * lighting.ambientColor.a;
    vec3 ambientSpecular = ambientFresnel * lighting.environmentColor.rgb * lighting.environmentColor.a * ambientSpecularStrength;
    vec3 ambient = (ambientDiffuse + ambientSpecular) * occlusion;
#if !RENDERER_BINDLESS // 개별 바인딩 경로에서만 명명된 재질·화면 환경광 자원을 포함한다. bindless 경로는 재질 배열을 사용한다.
    // 장면 픽셀 좌표에 화면 역크기를 곱해 화면 AO·환경광 결과를 읽는다. 이 자원들은 개별 텍스처 경로에만 연결된다.
    vec2 screenUV=gl_FragCoord.xy*environment.screen.xy;
    float screenAO=environment.screen.z>0.5 ? texture(sampler2D(screenAOTexture,screenSampler),screenUV).r : 1.0;
    ambient*=screenAO;
    if(environment.settings.x>0.5) // 환경광 패스가 활성화되면 기존 단색 주변광을 0으로 지우고, 알파 모드에 따라 SH 확산광 또는 화면 환경광 텍스처 결과로 다시 계산한다.
    {
        ambient=vec3(0);
        if(alphaMode==2u) // 투명 표면은 불투명 표면용 화면 환경광 대신 현재 법선의 SH 확산 경로로 간다.
        {
            if((uint(environment.screen.w+0.5)&1u)!=0u) // 환경 diffuse 비트가 켜져 있을 때만 투명 표면의 SH 확산광을 더한다.
                ambient=kD*albedo*EnvironmentIrradiance(normal)*(0.3183098861837907*environment.settings.y*occlusion*screenAO);
        }
        else
        {
            // 불투명 표면은 앞선 환경광 패스가 만든 화면 확산·반사 결과를 읽는다. 확산에는 현재 재질의 kD·albedo를 곱하고 반사는 그대로 더하며, 여기서 AO를 다시 곱하지 않는다.
            ambient=kD*albedo*texture(sampler2D(environmentDiffuseTexture,screenSampler),screenUV).rgb;
            ambient+=texture(sampler2D(environmentSpecularTexture,screenSampler),screenUV).rgb;
        }
    }
#endif
    vec3 emissive = pushConstants.emissiveColor.rgb;
    if ((textureFlags & RENDERER_TEXTURE_FLAG_EMISSIVE) != 0) { // 발광 이미지가 있으면 발광 상수 RGB에 이미지를 곱한다.
        emissive *= SampleMaterial(4u,fragUv).rgb;
    }
    vec3 color = lighting.lightCounts.w > 0.5 ? ambient + directLight + emissive : albedo + emissive;

    if(lighting.pbrParams.z >= 0.0) { // 직접 출력 경로만 노출과 톤 매핑을 적용한다. 음수는 선형 HDR로 남겨 후단 출력 패스에 맡긴다.
        color = min(color * lighting.pbrParams.w, vec3(16777216.0));
    color = color / (color + vec3(1.0));
    if (lighting.pbrParams.z > 0.5) color = pow(color, vec3(1.0 / 2.2)); // 이 출력 경로가 감마 적용을 요구할 때만 RGB에 1/2.2 거듭제곱을 적용한다.
    }
    // HDR 장면의 RGBA16F 저장 한도 65504로 RGB를 제한하고 NaN은 0으로 치환한다. 넘친 밝기가 후속 GI·bloom·시간 누적 통계까지 오염되는 일을 줄인다.
    color=vec3(isnan(color.r)?0.0:clamp(color.r,0.0,65504.0),
        isnan(color.g)?0.0:clamp(color.g,0.0,65504.0),isnan(color.b)?0.0:clamp(color.b,0.0,65504.0));
    outColor = vec4(color, alphaMode==2u ? base.a : 1.0);
}
