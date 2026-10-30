#define RENDERER_MAX_DIRECTIONAL_LIGHTS 4
#define RENDERER_MAX_POINT_LIGHTS 16
#define RENDERER_MAX_SPOT_LIGHTS 16

#ifndef RENDERER_ENABLE_SHADOWS // 그림자 사용 여부를 파이프라인이 명시하지 않았으면 컴파일 오류를 내어 자원 배치 불일치를 막는다.
#error RENDERER_ENABLE_SHADOWS must be defined
#endif

// 정점·픽셀 단계가 공유하는 192바이트 draw 상수다. 행렬 뒤의 재질 필드도 생략하지 않아 CPU와 같은 위치에서 읽는다.
// 표면에 수직인 방향(법선)의 Y축 반전 여부를 padding1에 넣는다.
// padding0은 알파 임곗값 비트, padding1은 알파 모드·양면·법선 Y 반전·unlit·반전 변환 면 방향 보정 비트, padding2는 재질 번호다. 실제 재질 판정은 픽셀 단계가 맡는다.
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

#if RENDERER_ENABLE_SHADOWS // 그림자 사용 빌드에서만 공유 그림자 자료와 광원 공간 입출력을 포함한다.
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
#endif

// 표면을 따라가는 방향(접선)을 정점 속성으로 받는다.
// 한 정점은 위치·법선·UV·접선·색상의 64바이트 배치다. 색상은 8번 입력, 인스턴싱 모델 행렬의 네 열은 4~7번 입력을 쓴다.
struct VSInput
{
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 tangent : TEXCOORD3;
    float4 color : TEXCOORD8;
#if RENDERER_INSTANCING
    float4 model0 : TEXCOORD4;
    float4 model1 : TEXCOORD5;
    float4 model2 : TEXCOORD6;
    float4 model3 : TEXCOORD7;
#endif
};

// clip 위치는 래스터라이저에, 나머지는 보간되어 픽셀 단계에 전달된다. 접선 w에는 법선 맵의 좌우 방향 부호를 보관한다.
struct VSOutput
{
    precise float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float3 worldPosition : TEXCOORD1;
    float3 worldNormal : TEXCOORD2;
    float4 worldTangent : TEXCOORD3;
    float4 color : TEXCOORD5;
#if RENDERER_ENABLE_SHADOWS
    float4 lightSpacePosition : TEXCOORD4;
#endif
};

// 아래 MeshNormalMatrix는 모델 행렬에서 역변환을 만들 수 없으면 법선 변환만 단위행렬로 대신하여 입력 법선 방향을 유지한다.
// 가장 큰 절댓값으로 먼저 나누어 길이 제곱 계산의 넘침을 줄인다. 방향을 만들 수 없는 입력은 호출자가 준 대체 방향을 반환한다.
float3 MeshNormalizeOr(float3 value, float3 fallback)
{
    float scale = max(max(abs(value.x),abs(value.y)),abs(value.z));
    if (!(scale > 0.0) || scale > 3.402823466e38) return fallback; // 유효한 양의 크기가 아니거나 float 범위를 넘으면 대체 방향을 쓴다.
    value /= scale;
    return value / length(value);
}

// 법선과 거의 평행하지 않은 기준축을 고르고, 두 벡터 모두에 수직인 벡터를 구하는 연산(외적)을 적용한다. 원래 접선을 사용할 수 없을 때 법선에 수직인 접선으로 대신한다.
float3 MeshFallbackTangent(float3 normal)
{
    float3 up = abs(normal.z) < 0.999 ? float3(0,0,1) : float3(0,1,0);
    return MeshNormalizeOr(cross(up,normal),float3(1,0,0));
}

// 행렬 변환이 넓이·부피를 얼마나 바꾸고 방향을 뒤집는지 나타내는 값(행렬식)을 이용한다.
// 특정 행과 열을 제외한 작은 행렬의 행렬식에 위치에 따른 부호를 붙인 값(여인수)으로 법선 변환 행렬을 만들어 비균일 확대와 반전을 반영한다.
// 축을 공통 배율로 줄인 뒤 외적하고 행렬식 부호를 반영한다. 최종 법선은 다시 정규화하므로 공통 양의 역배율은 생략한다.
float3x3 MeshNormalMatrix(float4x4 matrix)
{
    // HLSL의 matrix[행][열]에서 각 열을 모은다. 반환할 때도 행 생성자의 결과를 transpose하여 같은 열 기준 법선 행렬을 만든다.
    float3 a = float3(matrix[0][0], matrix[1][0], matrix[2][0]);
    float3 b = float3(matrix[0][1], matrix[1][1], matrix[2][1]);
    float3 c = float3(matrix[0][2], matrix[1][2], matrix[2][2]);
    float scale = max(max(max(abs(a.x),abs(a.y)),abs(a.z)),
        max(max(max(abs(b.x),abs(b.y)),abs(b.z)),max(max(abs(c.x),abs(c.y)),abs(c.z))));
    if (scale == 0.0) return float3x3(1,0,0, 0,1,0, 0,0,1); // 세 축이 모두 0이면 법선 변환을 단위행렬로 대신한다.
    a /= scale; b /= scale; c /= scale;
    float det = dot(a, cross(b, c));
    if (det == 0.0) return float3x3(1,0,0, 0,1,0, 0,0,1); // 행렬식이 0인 특이 변환은 역변환을 만들 수 없어 단위행렬로 대신한다.
    float orientation = det < 0.0 ? -1.0 : 1.0;
    return transpose(float3x3(cross(b, c) * orientation, cross(c, a) * orientation, cross(a, b) * orientation));
}

// 모델 행렬 선택 → 월드·clip 위치 계산 → 법선 변환 → 접선 직교화 순서다. 확대·반전 뒤에도 법선 맵의 기준축을 일관되게 유지한다.
VSOutput main(VSInput input)
{
#if RENDERER_INSTANCING // 인스턴싱 빌드에서만 물체별 행렬 입력을 사용하며, 일반 경로는 draw 상수의 모델 행렬을 쓴다.
    // 인스턴스 입력은 네 열이다. HLSL 생성자는 네 벡터를 행으로 놓으므로 transpose로 원래 모델 행렬을 복원한다.
    const float4x4 world = transpose(float4x4(input.model0,input.model1,input.model2,input.model3));
#else
    const float4x4 world = modelMatrix;
#endif
    // 월드 변환과 카메라 변환을 나누어 계산한다. GLSL/HLSL의 precise 및 GLSL/Metal 위치 출력의 invariant는 패스 사이 위치 계산의 일관성을 위한 표기다.
    precise float4 worldPosition = mul(world, float4(input.position, 1.0));
    precise float4 clipPosition = mul(viewProjectionMatrix, worldPosition);
    const float3x3 normalMatrix = MeshNormalMatrix(world);

    VSOutput output;
    output.position = clipPosition;
    output.uv = input.uv;
    output.color = input.color;
    output.worldPosition = worldPosition.xyz;
    output.worldNormal = MeshNormalizeOr(mul(normalMatrix, input.normal), float3(0,0,1));
    float3 tangent = mul((float3x3)world, input.tangent.xyz);
    // 접선에서 법선 방향 성분을 빼 직교화한다. 길이가 사라지면 대체 접선을 쓰고, 다음 줄에서 모델 반전의 부호를 접선 w에 반영한다.
    tangent = MeshNormalizeOr(tangent - output.worldNormal * dot(output.worldNormal, tangent), MeshFallbackTangent(output.worldNormal));
    output.worldTangent = float4(tangent, determinant((float3x3)world) < 0.0 ? -input.tangent.w : input.tangent.w);
#if RENDERER_ENABLE_SHADOWS // 그림자 사용 빌드에서만 공유 그림자 자료와 광원 공간 입출력을 포함한다.
    // 호환 입출력으로 0번 시점 위치를 넘긴다. 현재 픽셀 그림자 계산은 광원·캐스케이드에 맞는 행렬로 월드 위치를 다시 투영한다.
    output.lightSpacePosition = mul(lightViewProjectionMatrix[0], worldPosition);
#endif
    return output;
}
