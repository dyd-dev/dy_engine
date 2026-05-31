#pragma once

#include <cstdint>

#include "Math/Math.h"

namespace dy::Graphics
{
	struct ShadowMapDesc
	{
		uint32_t resolution = 2048;
		float orthoWidth = 6.0f;
		float orthoHeight = 6.0f;
		float nearPlane = 0.1f;
		float farPlane = 20.0f;
		float spotFovYRadians = 1.57079633f;
		Math::float3 sceneCenter = Math::float3(0.0f, 0.0f, 0.0f);
		float lightDistance = 8.0f;
	};

	[[nodiscard]] Math::float4x4 ComputeDirectionalLightViewProj(
		const Math::float3& lightDirection,
		const ShadowMapDesc& desc);

	[[nodiscard]] Math::float4x4 ComputeSpotLightViewProj(
		const Math::float3& lightPosition,
		const Math::float3& lightDirection,
		const ShadowMapDesc& desc);

	[[nodiscard]] ShadowMapDesc FitDirectionalShadowMapToBounds(
		const Math::float3& lightDirection,
		const ShadowMapDesc& baseDesc,
		const Math::float3& boundsMin,
		const Math::float3& boundsMax,
		float padding);
}
