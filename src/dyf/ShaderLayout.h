#pragma once

#include <cstddef>
#include <cstdint>

#include <array>
#include "dyf/Math/Math.h"
// 여러 렌더링 단계가 같은 셰이더 메모리 배치를 사용한다. 바인딩은 각 RHI 호출에 명시한다.

namespace dyf
{
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
		} directionalLights[4];
		struct
		{
			Math::float4 positionRange;
			Math::float4 colorIntensity;
		} pointLights[16];
		struct
		{
			Math::float4 positionRange;
			Math::float4 directionOuterCos;
			Math::float4 colorIntensity;
			Math::float4 coneParams;
		} spotLights[16];
		struct
		{
			Math::float4 positionIntensity;
			Math::float4 directionWidth;
			Math::float4 upHeight;
			Math::float4 color;
		} rectAreaLights[4];
		struct
		{
			Math::float4 positionIntensity;
			Math::float4 directionRadius;
			Math::float4 up;
			Math::float4 color;
		} discAreaLights[4];
		Math::float4 shadowLight;
	};
	static_assert(sizeof(RendererLightingConstants) == 2368);

    // 고정 조명 상수의 ABI를 바꾸지 않고 전체 점광원·스폿광원을 별도 저장한다.
    struct RendererClusterLight
    {
        Math::float4 positionRange;
        Math::float4 colorIntensity;
        Math::float4 directionOuterCos;
        Math::float4 coneShadow; // 내부 코사인, 광원 종류(2/3), 기존 그림자 슬롯(-1이면 없음), 예약
    };
    struct RendererClusterConstants
    {
        Math::float4x4 view, projection, inverseProjection;
        Math::float4 depth; // 근거리, 원거리, log(원거리/근거리), 뷰 변환의 보수적 반경 배율
        std::array<uint32_t,4> dimensions = {16,9,24,64};
        std::array<uint32_t,4> counts = {}; // 전체 광원 수, 클러스터 사용, 원근 투영, 예약
    };
    static_assert(sizeof(RendererClusterLight) == 64);
    static_assert(sizeof(RendererClusterConstants) == 240);

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
	static_assert(offsetof(RendererShadowConstants, viewProjectionParams) == 10960u);
	static_assert(sizeof(RendererShadowConstants) == 13024u);

	struct DrawConstants
	{
		Math::float4x4 viewProjectionMatrix;
		Math::float4x4 modelMatrix;
		uint32_t textureFlags = 0;
		// Reserved words retain the 192-byte shader-override ABI. Stock shaders use
		// padding0 as alphaCutoff float bits; padding1: alpha mode | double-sided (4) | normal Y flip (8).
		uint32_t padding0 = 0;
		uint32_t padding1 = 0;
		uint32_t padding2 = 0;
		Math::float4 emissiveColor;
		Math::float4 baseColor;
		Math::float4 materialParams;
	};

	static_assert(offsetof(DrawConstants, textureFlags) == 128u);
	static_assert(offsetof(DrawConstants, emissiveColor) == 144u);
	static_assert(offsetof(DrawConstants, baseColor) == 160u);
	static_assert(offsetof(DrawConstants, materialParams) == 176u);
	static_assert(sizeof(DrawConstants) == 192u);
}
