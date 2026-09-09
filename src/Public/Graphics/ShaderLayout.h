#pragma once

#include <cstddef>
#include <cstdint>

#include "Math/Math.h"
#include "Graphics/ShaderInterop/StockShaderLayout.inc"

namespace dy::Graphics::ShaderLayout
{
	struct CanvasVertex { float x, y, u, v, r, g, b, a; };

	inline constexpr uint32_t kMaxDirectionalLights = RENDERER_MAX_DIRECTIONAL_LIGHTS;
	inline constexpr uint32_t kMaxPointLights = RENDERER_MAX_POINT_LIGHTS;
	inline constexpr uint32_t kMaxSpotLights = RENDERER_MAX_SPOT_LIGHTS;
	inline constexpr uint32_t kMaxRectAreaLights = RENDERER_MAX_RECT_AREA_LIGHTS;
	inline constexpr uint32_t kMaxDiscAreaLights = RENDERER_MAX_DISC_AREA_LIGHTS;
#define DY_LIGHT_FLOAT4 Math::float4
#include "Graphics/ShaderInterop/LightingTypes.inc"
#undef DY_LIGHT_FLOAT4
	static_assert(sizeof(RendererLightingConstants) == 2368);

	struct RendererShadowConstants
	{
		Math::float4x4 lightViewProjectionMatrix;
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
}
