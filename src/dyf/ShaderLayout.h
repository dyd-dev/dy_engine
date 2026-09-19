#pragma once

#include <cstddef>
#include <cstdint>

#include "dyf/Renderer.h"
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
	};

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

	struct RendererShadowConstants
	{
    Math::float4x4 lightViewProjectionMatrix[128];
    Math::float4 atlasRect[128];
    Math::float4 directionalViews[4];
    Math::float4 pointViews[16];
    Math::float4 spotViews[16];
    Math::float4 directionalSplits[4];
    Math::float4x4 cameraView;
    Math::float4 filterParams;
	};

	struct DrawConstants
	{
		Math::float4x4 viewProjectionMatrix;
		Math::float4x4 modelMatrix;
		uint32_t textureFlags = 0;
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
// 현재 프레임의 그림자 행렬과 아틀라스 배치다. GPU 자원은 Renderer가 소유한다.
struct Renderer::ShadowData
{
    RendererShadowConstants constants={};
    uint32_t viewCount=0,columns=1,rows=1,resolution=1;
};
}
