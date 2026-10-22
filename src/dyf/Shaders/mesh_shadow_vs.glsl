#version 450

// 이 셰이더가 사용하는 바인딩과 입력 크기다.
#define RENDERER_DESCRIPTOR_SET 0
#define RENDERER_MAX_DIRECTIONAL_LIGHTS 4
#define RENDERER_MAX_POINT_LIGHTS 16
#define RENDERER_MAX_SPOT_LIGHTS 16

#define RENDERER_BINDING_SHADOW_MATRIX 3

// 위치와 UV만 필요하다. 표면에 수직인 방향(법선)·표면을 따라가는 방향(접선)·조명은 그림자 깊이 생성에 사용하지 않는다.
layout(location = 0) in vec3 inPosition;
layout(location = 2) in vec2 inUv;
layout(location = 0) out vec2 shadowUv;
#if RENDERER_INSTANCING // 인스턴싱 빌드에서만 물체별 행렬 입력을 사용하며, 일반 경로는 draw 상수의 모델 행렬을 쓴다.
layout(location = 4) in mat4 instanceModel;
#endif

// 정점·픽셀 단계가 공유하는 192바이트 draw 상수다. 행렬 뒤의 재질 필드도 생략하지 않아 CPU와 같은 위치에서 읽는다.
// 이 그림자 패스에서는 padding2가 lightViewProjectionMatrix의 시점 번호다. 일반 색상 패스에서의 재질 번호와 용도가 다르다.
layout(push_constant) uniform DrawConstants
{
    mat4 viewProjectionMatrix;
    mat4 modelMatrix;
    uint textureFlags;
    uint padding0;
    uint padding1;
    uint padding2;
    vec4 emissiveColor;
    vec4 baseColor;
    vec4 materialParams;
} drawConstants;

// 최대 128개 그림자 시점의 행렬·아틀라스 영역과 광원별 시점 범위를 공유한다.
// viewProjectionParams와 softShadowParams도 픽셀 단계의 깊이 복원·PCSS와 같은 배치를 유지한다.
layout(set = RENDERER_DESCRIPTOR_SET, binding = RENDERER_BINDING_SHADOW_MATRIX) uniform ShadowMatrix
{
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

// UV는 그대로 넘기고, 모델 행렬로 월드 위치를 만든 뒤 padding2가 가리키는 광원 행렬을 곱한다. 깊이 저장은 이후 래스터·깊이 단계가 맡는다.
void main()
{
    shadowUv=inUv;
#if RENDERER_INSTANCING // 인스턴싱 빌드에서만 물체별 행렬 입력을 사용하며, 일반 경로는 draw 상수의 모델 행렬을 쓴다.
    gl_Position = shadowMatrix.lightViewProjectionMatrix[drawConstants.padding2] * instanceModel * vec4(inPosition, 1.0);
#else
    gl_Position = shadowMatrix.lightViewProjectionMatrix[drawConstants.padding2] * drawConstants.modelMatrix * vec4(inPosition, 1.0);
#endif
}
