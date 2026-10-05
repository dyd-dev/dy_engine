#include <metal_stdlib>
// 이 셰이더가 사용하는 바인딩과 입력 크기다.
#define RENDERER_MAX_DIRECTIONAL_LIGHTS 4
#define RENDERER_MAX_POINT_LIGHTS 16
#define RENDERER_MAX_SPOT_LIGHTS 16

#define RENDERER_BINDING_SHADOW_MATRIX 3
#define RENDERER_BINDING_INLINE_CONSTANTS 10

#ifndef RENDERER_SHADOW_VERTEX_ENTRY
#define RENDERER_SHADOW_VERTEX_ENTRY shadowVertexShader
#endif

using namespace metal;

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
};

struct ShadowVertex
{
    float3 position [[attribute(0)]];
#if RENDERER_INSTANCING
    float4 model0 [[attribute(4)]];
    float4 model1 [[attribute(5)]];
    float4 model2 [[attribute(6)]];
    float4 model3 [[attribute(7)]];
#endif
};

vertex float4 RENDERER_SHADOW_VERTEX_ENTRY(
    ShadowVertex input [[stage_in]],
    constant DrawConstants& drawConstants [[buffer(RENDERER_BINDING_INLINE_CONSTANTS)]],
    constant ShadowMatrix& shadowMatrix [[buffer(RENDERER_BINDING_SHADOW_MATRIX)]])
{
#if RENDERER_INSTANCING
    const float4x4 world(input.model0, input.model1, input.model2, input.model3);
    return shadowMatrix.lightViewProjectionMatrix[drawConstants.padding2] * world * float4(input.position, 1.0f);
#else
    return shadowMatrix.lightViewProjectionMatrix[drawConstants.padding2] * drawConstants.modelMatrix * float4(input.position, 1.0f);
#endif
}
