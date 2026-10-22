// 이 셰이더가 사용하는 바인딩과 입력 크기다.
#define RENDERER_MAX_DIRECTIONAL_LIGHTS 4
#define RENDERER_MAX_POINT_LIGHTS 16
#define RENDERER_MAX_SPOT_LIGHTS 16

cbuffer DrawConstants : register(
    b10,
    space0)
{
    column_major float4x4 viewProjectionMatrix;
    column_major float4x4 modelMatrix;
    uint textureFlags;
    uint padding0;
    uint padding1;
    uint padding2;
    float4 emissiveColor;
    float4 baseColor;
    float4 materialParams;
};

cbuffer ShadowMatrix : register(
    b3,
    space0)
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

struct ShadowOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float alpha : TEXCOORD1;
};

#if RENDERER_INSTANCING
struct ShadowVertex
{
    float3 position : TEXCOORD0;
    float2 uv : TEXCOORD2;
    float alpha : TEXCOORD8;
    float4 model0 : TEXCOORD4;
    float4 model1 : TEXCOORD5;
    float4 model2 : TEXCOORD6;
    float4 model3 : TEXCOORD7;
};

ShadowOutput main(ShadowVertex input)
{
    const float4x4 world = transpose(float4x4(input.model0, input.model1, input.model2, input.model3));
    ShadowOutput output;
    output.position=mul(lightViewProjectionMatrix[padding2], mul(world, float4(input.position, 1.0)));
    output.uv=input.uv;
    output.alpha=input.alpha;
    return output;
}
#else
ShadowOutput main(float3 position : TEXCOORD0, float2 uv : TEXCOORD2, float alpha : TEXCOORD8)
{
    ShadowOutput output;
    output.position=mul(lightViewProjectionMatrix[padding2], mul(modelMatrix, float4(position, 1.0)));
    output.uv=uv;
    output.alpha=alpha;
    return output;
}
#endif
