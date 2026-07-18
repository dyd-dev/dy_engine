#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "Graphics/RendererConfig.h"
#include "Graphics/RendererShaderLayout.h"
#include "Graphics/Scene.h"

namespace dy::Graphics
{
	enum class ShadowLightType : uint32_t
	{
		None = 0u,
		Directional = 1u,
		Point = 2u,
		Spot = 3u
	};

	struct ShadowLightSelection
	{
		ShadowLightType type = ShadowLightType::None;
		uint32_t packedIndex = 0u;
		uint32_t sceneIndex = 0u;
		float strength = 0.0f;
	};

	namespace Detail
	{
		[[nodiscard]] inline Math::float3 NonNegative(const Math::float3& value)
		{
			return Math::float3(
				std::max(value.x, 0.0f),
				std::max(value.y, 0.0f),
				std::max(value.z, 0.0f));
		}

		[[nodiscard]] inline RendererShaderLayout::RendererDirectionalLight PackDirectionalLight(const DirectionalLight& light)
		{
			const Math::float3 direction = Math::NormalizeOr(light.direction, Math::float3(0.0f, 0.0f, 1.0f));
			const Math::float3 color = NonNegative(light.color);
			return {
				Math::float4(direction.x, direction.y, direction.z, std::max(light.illuminanceLux, 0.0f)),
				Math::float4(color.x, color.y, color.z, 0.0f)
			};
		}

		[[nodiscard]] inline RendererShaderLayout::RendererPointLight PackPointLight(const PointLight& light)
		{
			const Math::float3 color = NonNegative(light.color);
			return {
				Math::float4(light.position.x, light.position.y, light.position.z, std::max(light.rangeMeters, 0.0f)),
				Math::float4(color.x, color.y, color.z, std::max(light.luminousIntensityCandela, 0.0f))
			};
		}

		[[nodiscard]] inline RendererShaderLayout::RendererSpotLight PackSpotLight(const SpotLight& light)
		{
			constexpr float kMaxConeRadians = 1.55334306f;
			const float innerRadians = std::clamp(std::min(light.innerConeRadians, light.outerConeRadians), 0.0f, kMaxConeRadians);
			const float outerRadians = std::clamp(std::max(light.innerConeRadians, light.outerConeRadians), innerRadians, kMaxConeRadians);
			const Math::float3 direction = Math::NormalizeOr(light.direction, Math::float3(0.0f, 0.0f, -1.0f));
			const Math::float3 color = NonNegative(light.color);
			return {
				Math::float4(light.position.x, light.position.y, light.position.z, std::max(light.rangeMeters, 0.0f)),
				Math::float4(direction.x, direction.y, direction.z, std::cos(outerRadians)),
				Math::float4(color.x, color.y, color.z, std::max(light.luminousIntensityCandela, 0.0f)),
				Math::float4(std::cos(innerRadians), 0.0f, 0.0f, 0.0f)
			};
		}

		[[nodiscard]] inline Math::float3 OrthogonalUp(const Math::float3& direction, const Math::float3& up)
		{
			const Math::float3 projected = up - direction * Math::Dot(up, direction);
			const Math::float3 fallback = std::abs(direction.z) < 0.99f
				? Math::float3(0.0f, 0.0f, 1.0f)
				: Math::float3(0.0f, 1.0f, 0.0f);
			return Math::NormalizeOr(projected, Math::NormalizeOr(fallback - direction * Math::Dot(fallback, direction), Math::float3(0.0f, 1.0f, 0.0f)));
		}

		[[nodiscard]] inline RendererShaderLayout::RendererRectAreaLight PackRectAreaLight(const RectAreaLight& light)
		{
			const Math::float3 direction = Math::NormalizeOr(light.direction, Math::float3(0.0f, 0.0f, -1.0f));
			const Math::float3 up = OrthogonalUp(direction, light.up);
			const Math::float3 color = NonNegative(light.color);
			return {
				Math::float4(light.position.x, light.position.y, light.position.z, std::max(light.luminanceNits, 0.0f)),
				Math::float4(direction.x, direction.y, direction.z, std::max(light.widthMeters, 0.0f)),
				Math::float4(up.x, up.y, up.z, std::max(light.heightMeters, 0.0f)),
				Math::float4(color.x, color.y, color.z, 0.0f)
			};
		}

		[[nodiscard]] inline RendererShaderLayout::RendererDiscAreaLight PackDiscAreaLight(const DiscAreaLight& light)
		{
			const Math::float3 direction = Math::NormalizeOr(light.direction, Math::float3(0.0f, 0.0f, -1.0f));
			const Math::float3 up = OrthogonalUp(direction, light.up);
			const Math::float3 color = NonNegative(light.color);
			return {
				Math::float4(light.position.x, light.position.y, light.position.z, std::max(light.luminanceNits, 0.0f)),
				Math::float4(direction.x, direction.y, direction.z, std::max(light.radiusMeters, 0.0f)),
				Math::float4(up.x, up.y, up.z, 0.0f),
				Math::float4(color.x, color.y, color.z, 0.0f)
			};
		}
	}

	[[nodiscard]] inline RendererShaderLayout::RendererLightingConstants BuildRendererLightingConstants(
		const Scene& scene,
		const RendererDesc& config,
		bool shadowsEnabled,
		ShadowLightSelection* outShadowSelection = nullptr)
	{
		namespace Layout = RendererShaderLayout;
		Layout::RendererLightingConstants lighting = {};
		lighting.cameraPosition = Math::float4(config.cameraPosition.x, config.cameraPosition.y, config.cameraPosition.z, 1.0f);
		lighting.ambientColor = Math::float4(
			config.ambientColor.x * config.environment.diffuseColor.x,
			config.ambientColor.y * config.environment.diffuseColor.y,
			config.ambientColor.z * config.environment.diffuseColor.z,
			std::max(config.ambientIntensity * config.environment.diffuseIntensity, 0.0f));
		lighting.shadowParams = Math::float4(
			std::max(config.shadowDepthBias, 0.0f),
			std::max(config.shadowSlopeBias, 0.0f),
			std::max(config.shadowNormalBias, 0.0f),
			static_cast<float>(config.shadowPcfRadius));
		lighting.pbrParams = Math::float4(
			std::clamp(config.pbr.minRoughness, 0.01f, 1.0f),
			std::max(config.pbr.ambientSpecularStrength, 0.0f),
			config.enableHdrRendering || RHI::IsSrgbFormat(config.renderTargetFormat) ? 0.0f : 1.0f,
			config.enableHdrRendering ? 0.0f : 1.0f);
		lighting.environmentColor = Math::float4(
			config.environment.specularColor.x,
			config.environment.specularColor.y,
			config.environment.specularColor.z,
			std::max(config.environment.specularIntensity, 0.0f));

		const std::vector<uint32_t> directionalIndices = SelectActiveLightIndices(
			scene.DirectionalLights(), Layout::kMaxDirectionalLights);
		const std::vector<uint32_t> pointIndices = SelectActiveLightIndices(
			scene.PointLights(), Layout::kMaxPointLights);
		const std::vector<uint32_t> spotIndices = SelectActiveLightIndices(
			scene.SpotLights(), Layout::kMaxSpotLights);
		const std::vector<uint32_t> rectAreaIndices = SelectActiveLightIndices(
			scene.RectAreaLights(), Layout::kMaxRectAreaLights);
		const std::vector<uint32_t> discAreaIndices = SelectActiveLightIndices(
			scene.DiscAreaLights(), Layout::kMaxDiscAreaLights);

		const bool hasAnySceneDirectLight =
			scene.GetDirectionalLightCount() > 0u ||
			scene.GetPointLightCount() > 0u ||
			scene.GetSpotLightCount() > 0u;
		uint32_t directionalCount = 0u;
		if (!hasAnySceneDirectLight)
		{
			DirectionalLight fallback;
			fallback.direction = config.directionalLightDirection;
			fallback.color = config.directionalLightColor;
			fallback.illuminanceLux = config.directionalLightIntensity;
			fallback.shadowStrength = config.shadowStrength;
			lighting.directionalLights[0] = Detail::PackDirectionalLight(fallback);
			directionalCount = 1u;
		}
		else
		{
			directionalCount = static_cast<uint32_t>(directionalIndices.size());
			for (uint32_t packedIndex = 0u; packedIndex < directionalCount; ++packedIndex)
			{
				lighting.directionalLights[packedIndex] = Detail::PackDirectionalLight(scene.GetDirectionalLight(directionalIndices[packedIndex]));
			}
		}

		const uint32_t pointCount = static_cast<uint32_t>(pointIndices.size());
		for (uint32_t packedIndex = 0u; packedIndex < pointCount; ++packedIndex)
		{
			lighting.pointLights[packedIndex] = Detail::PackPointLight(scene.GetPointLight(pointIndices[packedIndex]));
		}
		const uint32_t spotCount = static_cast<uint32_t>(spotIndices.size());
		for (uint32_t packedIndex = 0u; packedIndex < spotCount; ++packedIndex)
		{
			lighting.spotLights[packedIndex] = Detail::PackSpotLight(scene.GetSpotLight(spotIndices[packedIndex]));
		}
		lighting.lightCounts = Math::float4(
			static_cast<float>(directionalCount),
			static_cast<float>(pointCount),
			static_cast<float>(spotCount),
			0.0f);
		const uint32_t rectAreaCount = static_cast<uint32_t>(rectAreaIndices.size());
		for (uint32_t packedIndex = 0u; packedIndex < rectAreaCount; ++packedIndex)
		{
			lighting.rectAreaLights[packedIndex] = Detail::PackRectAreaLight(scene.GetRectAreaLight(rectAreaIndices[packedIndex]));
		}
		const uint32_t discAreaCount = static_cast<uint32_t>(discAreaIndices.size());
		for (uint32_t packedIndex = 0u; packedIndex < discAreaCount; ++packedIndex)
		{
			lighting.discAreaLights[packedIndex] = Detail::PackDiscAreaLight(scene.GetDiscAreaLight(discAreaIndices[packedIndex]));
		}
		lighting.areaLightCounts = Math::float4(
			static_cast<float>(rectAreaCount),
			static_cast<float>(discAreaCount),
			0.0f,
			0.0f);

		ShadowLightSelection shadowSelection;
		if (shadowsEnabled)
		{
			if (!hasAnySceneDirectLight)
			{
				shadowSelection = { ShadowLightType::Directional, 0u, 0u, std::clamp(config.shadowStrength, 0.0f, 1.0f) };
			}
			else
			{
				for (uint32_t packedIndex = 0u; packedIndex < directionalCount; ++packedIndex)
				{
					const uint32_t sceneIndex = directionalIndices[packedIndex];
					const DirectionalLight& light = scene.GetDirectionalLight(sceneIndex);
					if (!light.castShadow) continue;
					shadowSelection = { ShadowLightType::Directional, packedIndex, sceneIndex, std::clamp(light.shadowStrength, 0.0f, 1.0f) };
					break;
				}
			}
			if (shadowSelection.type == ShadowLightType::None)
			{
				for (uint32_t packedIndex = 0u; packedIndex < spotCount; ++packedIndex)
				{
					const uint32_t sceneIndex = spotIndices[packedIndex];
					const SpotLight& light = scene.GetSpotLight(sceneIndex);
					if (!light.castShadow) continue;
					shadowSelection = { ShadowLightType::Spot, packedIndex, sceneIndex, std::clamp(light.shadowStrength, 0.0f, 1.0f) };
					break;
				}
			}
			if (shadowSelection.type == ShadowLightType::None)
			{
				for (uint32_t packedIndex = 0u; packedIndex < pointCount; ++packedIndex)
				{
					const uint32_t sceneIndex = pointIndices[packedIndex];
					const PointLight& light = scene.GetPointLight(sceneIndex);
					if (!light.castShadow) continue;
					shadowSelection = { ShadowLightType::Point, packedIndex, sceneIndex, std::clamp(light.shadowStrength, 0.0f, 1.0f) };
					break;
				}
			}
		}
		lighting.shadowLight = Math::float4(
			static_cast<float>(shadowSelection.type),
			static_cast<float>(shadowSelection.packedIndex),
			shadowSelection.strength,
			shadowSelection.type == ShadowLightType::None ? 0.0f : 1.0f);
		if (outShadowSelection != nullptr) *outShadowSelection = shadowSelection;
		return lighting;
	}
}
