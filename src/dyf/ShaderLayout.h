#pragma once

#include <cstddef>
#include <cstdint>

#include <array>
#include "dyf/Math/Math.h"
// 여러 렌더링 단계가 같은 셰이더 메모리 배치를 사용한다. 바인딩은 각 RHI 호출에 명시한다.

namespace dyf
{
	// 표면에 수직인 방향(법선)과 표면을 따라가는 방향(접선)을 정점 자료에 담는다.
	// float 개수는 위치 3·법선 3·UV 2·접선 4다. 접선의 xyz는 방향이고 w는 종법선 방향 부호에 쓰인다.
	struct RendererVertex
	{
		float px = 0.0f;
		float py = 0.0f;
		float pz = 0.0f;
		float nx = 0.0f;
		float ny = 0.0f;
		float nz = 1.0f;
		float u = 0.0f;
		float v = 0.0f;
		float tx = 1.0f;
		float ty = 0.0f;
		float tz = 0.0f;
		float tw = 1.0f;
		float cr = 1.0f;
		float cg = 1.0f;
		float cb = 1.0f;
		float ca = 1.0f;
	};
	static_assert(sizeof(RendererVertex) == 64 && offsetof(RendererVertex, cr) == 48);

	// 카메라·환경·기본 조명과 고정 용량 광원 배열을 float4 단위로 전달한다. 실제 선택한 광원 수는 같은 구조체의 lightCounts와 areaLightCounts에 담는다.
	struct RendererLightingConstants
	{
		Math::float4 cameraPosition;
		Math::float4 directionalLightDirection;
		Math::float4 directionalLightColor;
		Math::float4 ambientColor;
		Math::float4 shadowParams;
		Math::float4 pbrParams;
		Math::float4 environmentColor;
		Math::float4 pointLightPositionRange;
		Math::float4 pointLightColorIntensity;
		Math::float4 lightCounts;
		Math::float4 areaLightCounts;
		struct
		{
			Math::float4 directionIntensity;
			Math::float4 color;
		// 방향광은 최대 네 슬롯이며 각 슬롯이 방향/세기와 색을 보관한다.
		} directionalLights[4];
		struct
		{
			Math::float4 positionRange;
			Math::float4 colorIntensity;
		// 고정 경로 점광원은 최대 16개이고 위치/범위·색/세기를 묶는다.
		} pointLights[16];
		struct
		{
			Math::float4 positionRange;
			Math::float4 directionOuterCos;
			Math::float4 colorIntensity;
			Math::float4 coneParams;
		// 스폿은 위치/범위·방향/바깥 코사인·색/세기·원뿔 파라미터를 전달한다.
		} spotLights[16];
		struct
		{
			Math::float4 positionIntensity;
			Math::float4 directionWidth;
			Math::float4 upHeight;
			Math::float4 color;
		// 사각 면광원은 위치·방향·가로/세로·색으로 면의 기준을 구성한다.
		} rectAreaLights[4];
		struct
		{
			Math::float4 positionIntensity;
			Math::float4 directionRadius;
			Math::float4 up;
			Math::float4 color;
		// 원형 면광원은 방향·up 기준과 반지름을 전달한다.
		} discAreaLights[4];
		Math::float4 shadowLight;
	};
	static_assert(sizeof(RendererLightingConstants) == 2368);

    // 클러스터 경로의 전체 점광원·스폿광원 자료는 별도 버퍼로 전달해 고정 조명 상수의 ABI를 유지한다.
    struct RendererClusterLight
    {
        Math::float4 positionRange;
        Math::float4 colorIntensity;
        Math::float4 directionOuterCos;
        Math::float4 coneShadow; // 내부 코사인, 광원 종류(2/3), 기존 그림자 슬롯(-1이면 없음), 예약
    };
    // 뷰 변환과 깊이 분할, 16×9×24 기본 격자 및 셀당 64개 용량을 전달한다. 실제 사용 여부는 counts에 있다.
    struct RendererClusterConstants
    {
        Math::float4x4 view, projection, inverseProjection;
        Math::float4 depth; // 근거리, 원거리, log(원거리/근거리), 뷰 변환의 보수적 반경 배율
        std::array<uint32_t,4> dimensions = {16,9,24,64};
        std::array<uint32_t,4> counts = {}; // 전체 광원 수, 클러스터 사용, 원근 투영, 예약
    };
    static_assert(sizeof(RendererClusterLight) == 64);
    static_assert(sizeof(RendererClusterConstants) == 240);

	// 최대 128개 그림자 뷰의 투영과 atlas 영역, 광원별 뷰 연결·분할·필터 파라미터를 전달한다.
	struct RendererShadowConstants
	{
    Math::float4x4 lightViewProjectionMatrix[128];
    Math::float4 atlasRect[128];
    Math::float4 directionalViews[4];
    Math::float4 pointViews[16];
    Math::float4 spotViews[16];
    Math::float4 directionalSplits[4];
    Math::float4x4 cameraView;
    Math::float4 filterParams; // 차폐물 표본 수, 필터 표본 수, 기존 PCF 반경, 카메라 near
		// 기존 상수의 오프셋을 유지하며 UV 배율과 깊이 복원 계수를 뒤에 덧붙인다.
		Math::float4 viewProjectionParams[128]; // UV 배율 xy, 직교 깊이 폭 또는 원근 A/B
		Math::float4 softShadowParams; // 최대 필터 반경(타일 텍셀), 예약
	};
	// 확장된 깊이 복원 자료가 기존 그림자 상수 뒤에 붙었는지 오프셋을 고정한다.
	static_assert(offsetof(RendererShadowConstants, viewProjectionParams) == 10960u);
	static_assert(sizeof(RendererShadowConstants) == 13024u);

	// 한 Draw의 카메라/모델 행렬과 재질 상수다. 인스턴싱 경로는 개별 모델 정보 일부를 별도 버퍼에서 공급한다.
	struct DrawConstants
	{
		Math::float4x4 viewProjectionMatrix;
		Math::float4x4 modelMatrix;
		uint32_t textureFlags = 0;
		// Reserved words retain the 192-byte shader-override ABI. Stock shaders use
		// padding0 as alphaCutoff float bits; padding1: alpha mode | double-sided (4) | normal Y flip (8).
		// 이름은 예약 필드지만 내장 셰이더는 alphaCutoff의 float 비트를 그대로 저장한다. 정수로 수치 변환하지 않으며 셰이더에서도 같은 비트를 float로 다시 해석한다.
		uint32_t padding0 = 0;
		// 내장 경로는 알파 모드와 양면·법선 Y 반전 비트를 이 32비트 값에 묶는다.
		uint32_t padding1 = 0;
		uint32_t padding2 = 0;
		Math::float4 emissiveColor;
		Math::float4 baseColor;
		Math::float4 materialParams;
	};

	// 사용자 셰이더 재정의와의 192바이트 계약을 유지하도록 주요 필드의 위치와 전체 크기를 고정한다.
	static_assert(offsetof(DrawConstants, textureFlags) == 128u);
	static_assert(offsetof(DrawConstants, emissiveColor) == 144u);
	static_assert(offsetof(DrawConstants, baseColor) == 160u);
	static_assert(offsetof(DrawConstants, materialParams) == 176u);
	static_assert(sizeof(DrawConstants) == 192u);
}
