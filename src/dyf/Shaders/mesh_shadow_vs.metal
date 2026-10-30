#include <metal_stdlib>
// 이 셰이더가 사용하는 바인딩과 입력 크기다.
#define RENDERER_MAX_DIRECTIONAL_LIGHTS 4
#define RENDERER_MAX_POINT_LIGHTS 16
#define RENDERER_MAX_SPOT_LIGHTS 16

#define RENDERER_BINDING_SHADOW_MATRIX 3
#define RENDERER_BINDING_INLINE_CONSTANTS 10

#ifndef RENDERER_SHADOW_VERTEX_ENTRY // 별도 이름을 받지 않았으면 기본 그림자 정점 진입점 이름을 사용한다.
#define RENDERER_SHADOW_VERTEX_ENTRY shadowVertexShader
#endif

using namespace metal;

// 정점·픽셀 단계가 공유하는 192바이트 draw 상수다. 행렬 뒤의 재질 필드도 생략하지 않아 CPU와 같은 위치에서 읽는다.
// 이 그림자 패스에서는 padding2가 lightViewProjectionMatrix의 시점 번호다. 일반 색상 패스에서의 재질 번호와 용도가 다르다.
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

// 최대 128개 그림자 시점의 행렬·아틀라스 영역과 광원별 시점 범위를 공유한다.
// viewProjectionParams와 softShadowParams도 픽셀 단계의 깊이 복원·PCSS와 같은 배치를 유지한다.
struct ShadowMatrix
{
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

// 위치·UV와 8번 입력의 정점 알파를 사용한다. 법선·접선·조명은 그림자 깊이 생성에 사용하지 않는다.
struct ShadowVertex
{
    float3 position [[attribute(0)]];
    float2 uv [[attribute(2)]];
    float alpha [[attribute(8)]];
#if RENDERER_INSTANCING
    float4 model0 [[attribute(4)]];
    float4 model1 [[attribute(5)]];
    float4 model2 [[attribute(6)]];
    float4 model3 [[attribute(7)]];
#endif
};

struct ShadowOutput
{
    float4 position [[position]];
    float2 uv;
    float alpha;
};

// UV·정점 알파는 그대로 넘기고, 모델 행렬로 월드 위치를 만든 뒤 padding2가 가리키는 광원 행렬을 곱한다. 깊이 저장은 이후 래스터·깊이 단계가 맡는다.
vertex ShadowOutput RENDERER_SHADOW_VERTEX_ENTRY(
    ShadowVertex input [[stage_in]],
    constant DrawConstants& drawConstants [[buffer(RENDERER_BINDING_INLINE_CONSTANTS)]],
    constant ShadowMatrix& shadowMatrix [[buffer(RENDERER_BINDING_SHADOW_MATRIX)]])
{
    ShadowOutput output;
    output.uv=input.uv;
    output.alpha=input.alpha;
#if RENDERER_INSTANCING
    const float4x4 world(input.model0, input.model1, input.model2, input.model3);
    output.position=shadowMatrix.lightViewProjectionMatrix[drawConstants.padding2] * world * float4(input.position, 1.0f);
#else
    output.position=shadowMatrix.lightViewProjectionMatrix[drawConstants.padding2] * drawConstants.modelMatrix * float4(input.position, 1.0f);
#endif
    return output;
}
