#define RENDERER_MAX_DIRECTIONAL_LIGHTS 4
#define RENDERER_MAX_POINT_LIGHTS 16
#define RENDERER_MAX_SPOT_LIGHTS 16

// 정점·픽셀 단계가 공유하는 192바이트 draw 상수다. 행렬 뒤의 재질 필드도 생략하지 않아 CPU와 같은 위치에서 읽는다.
// 이 그림자 패스에서는 padding2가 lightViewProjectionMatrix의 시점 번호다. 일반 색상 패스에서의 재질 번호와 용도가 다르다.
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

// 최대 128개 그림자 시점의 행렬·아틀라스 영역과 광원별 시점 범위를 공유한다.
// viewProjectionParams와 softShadowParams도 픽셀 단계의 깊이 복원·PCSS와 같은 배치를 유지한다.
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
};

#if RENDERER_INSTANCING // 인스턴싱 빌드에서만 물체별 행렬 입력을 사용하며, 일반 경로는 draw 상수의 모델 행렬을 쓴다.
// 위치와 UV만 필요하다. 표면에 수직인 방향(법선)·표면을 따라가는 방향(접선)·조명은 그림자 깊이 생성에 사용하지 않는다.
struct ShadowVertex
{
    float3 position : TEXCOORD0;
    float2 uv : TEXCOORD2;
    float4 model0 : TEXCOORD4;
    float4 model1 : TEXCOORD5;
    float4 model2 : TEXCOORD6;
    float4 model3 : TEXCOORD7;
};

// UV는 그대로 넘기고, 모델 행렬로 월드 위치를 만든 뒤 padding2가 가리키는 광원 행렬을 곱한다. 깊이 저장은 이후 래스터·깊이 단계가 맡는다.
ShadowOutput main(ShadowVertex input)
{
    // CPU 인스턴스 버퍼의 네 열을 HLSL 행 생성자로 받으므로 transpose를 거쳐 모델 행렬을 복원한다.
    const float4x4 world = transpose(float4x4(input.model0, input.model1, input.model2, input.model3));
    ShadowOutput output;
    output.position=mul(lightViewProjectionMatrix[padding2], mul(world, float4(input.position, 1.0)));
    output.uv=input.uv;
    return output;
}
#else
// UV는 그대로 넘기고, 모델 행렬로 월드 위치를 만든 뒤 padding2가 가리키는 광원 행렬을 곱한다. 깊이 저장은 이후 래스터·깊이 단계가 맡는다.
ShadowOutput main(float3 position : TEXCOORD0, float2 uv : TEXCOORD2)
{
    ShadowOutput output;
    output.position=mul(lightViewProjectionMatrix[padding2], mul(modelMatrix, float4(position, 1.0)));
    output.uv=uv;
    return output;
}
#endif
