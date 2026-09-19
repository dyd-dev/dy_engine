#pragma once

#include <cstdint>

#include "dyf/Math/Math.h"

namespace dyf
{
	// 그림자 품질을 선택한다. 해상도와 필터 등 구현 값은 Renderer가 정한다.
	enum class ShadowQuality : uint8_t { Low, Medium, High };

	// 표면에 더할 환경색과 세기다. 생략하면 기본 환경을 사용한다.
	struct EnvironmentDesc
	{
		Math::float3 diffuseColor = Math::float3(1.0f, 1.0f, 1.0f);
		float diffuseIntensity = 1.0f;
		Math::float3 specularColor = Math::float3(1.0f, 1.0f, 1.0f);
		float specularIntensity = 1.0f;
	};

	// 원하는 조명 결과를 설정한다. 광원 종류와 표면 반응은 Light와 Material에서 정한다.
	struct LightingConfig
	{
		// 끄면 광원·환경광·그림자를 적용하지 않고 재질의 기본색과 발광을 표시한다.
		bool enabled = true;
		// 방향광·점광원·스폿 광원의 그림자를 켠다. 면적광 그림자는 아직 지원하지 않는다.
		bool shadows = false;
		ShadowQuality shadowQuality = ShadowQuality::Medium;
		Math::float3 ambientColor = Math::float3(1.0f, 1.0f, 1.0f);
		float ambientIntensity = 0.035f;
		EnvironmentDesc environment = {};
	};

	// 렌더러의 설정 값이다. 지정하지 않은 항목은 아래 기본값을 사용한다.
	struct RendererConfig
	{
		bool vsync = true;
		bool allowReadback = false;
		Math::float4 clearColor = Math::float4(0.08f, 0.10f, 0.14f, 1.0f);
		LightingConfig lighting = {};
		bool enableHdrRendering = false;
		float exposure = 1;
		// Scene/Mesh 렌더링의 HUD다. Canvas만 Render하는 경로에는 표시하지 않는다.
		bool enableProfilerHud = true;
		bool profilerStartsExpanded = false;
	};
}
