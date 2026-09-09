#pragma once
#include "Graphics/Private/RendererShaderLayout.h"
#include "Graphics/Scene.h"

namespace dy::Graphics::Private
{
	namespace LightingGpuDetail
	{
		[[nodiscard]] inline Math::float3 NormalizeDirection(const Math::float3& direction)
		{
			return Math::NormalizeOr(direction, Math::float3(0.0f, 0.0f, -1.0f));
		}

		[[nodiscard]] inline Math::float3 OrthogonalUp(const Math::float3& direction, const Math::float3& up)
		{
			const Math::float3 projected = up - direction * Math::Dot(up, direction);
			const Math::float3 fallback = std::fabs(direction.z) < 0.99f
				? Math::float3(0.0f, 0.0f, 1.0f)
				: Math::float3(0.0f, 1.0f, 0.0f);
			return Math::NormalizeOr(projected, Math::NormalizeOr(
				fallback - direction * Math::Dot(fallback, direction),
				Math::float3(1.0f, 0.0f, 0.0f)));
		}
	}


    inline void PackSceneLights(const Scene& scene, RendererShaderLayout::RendererLightingConstants& lighting)
    {
        namespace Layout = RendererShaderLayout;
        const auto directionalIndices=SelectActiveLightIndices(scene.DirectionalLights(),Layout::kMaxDirectionalLights);
        const auto pointIndices=SelectActiveLightIndices(scene.PointLights(),Layout::kMaxPointLights);
        const auto spotIndices=SelectActiveLightIndices(scene.SpotLights(),Layout::kMaxSpotLights);
        const auto rectAreaIndices=SelectActiveLightIndices(scene.RectAreaLights(),Layout::kMaxRectAreaLights);
        const auto discAreaIndices=SelectActiveLightIndices(scene.DiscAreaLights(),Layout::kMaxDiscAreaLights);
        const DirectionalLight* directional=directionalIndices.empty()?nullptr:&scene.GetDirectionalLight(directionalIndices[0]);
		const bool needsFallbackDirectional = false;
		const uint32_t directionalCount = directional != nullptr
			? static_cast<uint32_t>(directionalIndices.size())
			: (needsFallbackDirectional ? 1u : 0u);
		for(uint32_t index = 0u; index < directionalCount && directional != nullptr; ++index)
		{
			const DirectionalLight& light = scene.GetDirectionalLight(directionalIndices[index]);
			const Math::float3 direction = LightingGpuDetail::NormalizeDirection(light.direction);
			lighting.directionalLights[index] = {
				Math::float4(direction.x, direction.y, direction.z, std::max(light.intensity, 0.0f)),
				Math::float4(light.color.x, light.color.y, light.color.z, 0.0f)
			};
		}

		const uint32_t pointCount = static_cast<uint32_t>(pointIndices.size());
		for(uint32_t index = 0u; index < pointCount; ++index)
		{
			const PointLight& light = scene.GetPointLight(pointIndices[index]);
			lighting.pointLights[index] = {
				Math::float4(light.position.x, light.position.y, light.position.z, std::max(light.range, 0.0f)),
				Math::float4(light.color.x, light.color.y, light.color.z, std::max(light.intensity, 0.0f))
			};
		}

		const uint32_t spotCount = static_cast<uint32_t>(spotIndices.size());
		for(uint32_t index = 0u; index < spotCount; ++index)
		{
			const SpotLight& light = scene.GetSpotLight(spotIndices[index]);
			constexpr float kMaxConeRadians = 1.55334306f;
			const float innerRadians = std::clamp(std::min(light.innerConeRadians, light.outerConeRadians), 0.0f, kMaxConeRadians);
			const float outerRadians = std::clamp(std::max(light.innerConeRadians, light.outerConeRadians), innerRadians, kMaxConeRadians);
			const Math::float3 direction = LightingGpuDetail::NormalizeDirection(light.direction);
			lighting.spotLights[index] = {
				Math::float4(light.position.x, light.position.y, light.position.z, std::max(light.range, 0.0f)),
				Math::float4(direction.x, direction.y, direction.z, std::cos(outerRadians)),
				Math::float4(light.color.x, light.color.y, light.color.z, std::max(light.intensity, 0.0f)),
				Math::float4(std::cos(innerRadians), 0.0f, 0.0f, 0.0f)
			};
		}

		const uint32_t rectAreaCount = static_cast<uint32_t>(rectAreaIndices.size());
		for(uint32_t index = 0u; index < rectAreaCount; ++index)
		{
			const RectAreaLight& light = scene.GetRectAreaLight(rectAreaIndices[index]);
			const Math::float3 direction = LightingGpuDetail::NormalizeDirection(light.direction);
			const Math::float3 up = LightingGpuDetail::OrthogonalUp(direction, light.up);
			lighting.rectAreaLights[index] = {
				Math::float4(light.position.x, light.position.y, light.position.z, std::max(light.intensity, 0.0f)),
				Math::float4(direction.x, direction.y, direction.z, light.width),
				Math::float4(up.x, up.y, up.z, light.height),
				Math::float4(light.color.x, light.color.y, light.color.z, 0.0f)
			};
		}

		const uint32_t discAreaCount = static_cast<uint32_t>(discAreaIndices.size());
		for(uint32_t index = 0u; index < discAreaCount; ++index)
		{
			const DiscAreaLight& light = scene.GetDiscAreaLight(discAreaIndices[index]);
			const Math::float3 direction = LightingGpuDetail::NormalizeDirection(light.direction);
			const Math::float3 up = LightingGpuDetail::OrthogonalUp(direction, light.up);
			lighting.discAreaLights[index] = {
				Math::float4(light.position.x, light.position.y, light.position.z, std::max(light.intensity, 0.0f)),
				Math::float4(direction.x, direction.y, direction.z, light.radius),
				Math::float4(up.x, up.y, up.z, 0.0f),
				Math::float4(light.color.x, light.color.y, light.color.z, 0.0f)
			};
		}

		lighting.lightCounts = Math::float4(static_cast<float>(directionalCount), static_cast<float>(pointCount), static_cast<float>(spotCount), 0.0f);
		lighting.areaLightCounts = Math::float4(static_cast<float>(rectAreaCount), static_cast<float>(discAreaCount), 0.0f, 0.0f);

        lighting.shadowLight = {1,0,lighting.cameraPosition.w,lighting.directionalLightDirection.w};
    }
}
