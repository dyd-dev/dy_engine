#pragma once

#include <algorithm>
#include <cmath>

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

	[[nodiscard]] inline ShadowLightSelection SelectShadowLight(const Scene& scene)
	{
		namespace Layout = RendererShaderLayout;
		const std::vector<uint32_t> directional = SelectActiveLightIndices(scene.DirectionalLights(), Layout::kMaxDirectionalLights);
		for(uint32_t packed = 0u; packed < static_cast<uint32_t>(directional.size()); ++packed)
		{
			const DirectionalLight& light = scene.GetDirectionalLight(directional[packed]);
			if(light.castShadow) return { ShadowLightType::Directional, packed, directional[packed], std::clamp(light.shadowStrength, 0.0f, 1.0f) };
		}
		const std::vector<uint32_t> spots = SelectActiveLightIndices(scene.SpotLights(), Layout::kMaxSpotLights);
		for(uint32_t packed = 0u; packed < static_cast<uint32_t>(spots.size()); ++packed)
		{
			const SpotLight& light = scene.GetSpotLight(spots[packed]);
			if(light.castShadow) return { ShadowLightType::Spot, packed, spots[packed], std::clamp(light.shadowStrength, 0.0f, 1.0f) };
		}
		const std::vector<uint32_t> points = SelectActiveLightIndices(scene.PointLights(), Layout::kMaxPointLights);
		for(uint32_t packed = 0u; packed < static_cast<uint32_t>(points.size()); ++packed)
		{
			const PointLight& light = scene.GetPointLight(points[packed]);
			if(light.castShadow) return { ShadowLightType::Point, packed, points[packed], std::clamp(light.shadowStrength, 0.0f, 1.0f) };
		}
		return {};
	}

	[[nodiscard]] inline RendererShaderLayout::RendererLightingConstants BuildRendererLightingConstants(
		const Scene& scene,
		const RendererDesc& config,
		bool enableShadows,
		ShadowLightSelection* outShadowSelection = nullptr)
	{
		namespace Layout = RendererShaderLayout;
		const std::vector<uint32_t> directionalIndices = SelectActiveLightIndices(scene.DirectionalLights(), Layout::kMaxDirectionalLights);
		const std::vector<uint32_t> pointIndices = SelectActiveLightIndices(scene.PointLights(), Layout::kMaxPointLights);
		const std::vector<uint32_t> spotIndices = SelectActiveLightIndices(scene.SpotLights(), Layout::kMaxSpotLights);
		const std::vector<uint32_t> rectAreaIndices = SelectActiveLightIndices(scene.RectAreaLights(), Layout::kMaxRectAreaLights);
		const std::vector<uint32_t> discAreaIndices = SelectActiveLightIndices(scene.DiscAreaLights(), Layout::kMaxDiscAreaLights);
		const DirectionalLight* directional = directionalIndices.empty() ? nullptr : &scene.GetDirectionalLight(directionalIndices[0]);
		const PointLight* point = pointIndices.empty() ? nullptr : &scene.GetPointLight(pointIndices[0]);
		const Math::float3 lightDirection = directional != nullptr ? directional->direction : config.directionalLightDirection;
		const Math::float3 lightColor = directional != nullptr ? directional->color : config.directionalLightColor;
		const float lightIntensity = directional != nullptr ? directional->intensity : config.directionalLightIntensity;
		const bool hasAnySceneLight = !directionalIndices.empty() || !pointIndices.empty() || !spotIndices.empty()
			|| !rectAreaIndices.empty() || !discAreaIndices.empty();
		ShadowLightSelection shadowSelection = enableShadows ? SelectShadowLight(scene) : ShadowLightSelection{};
		if(enableShadows && !hasAnySceneLight)
		{
			shadowSelection = { ShadowLightType::Directional, 0u, 0u, std::clamp(config.shadowStrength, 0.0f, 1.0f) };
		}
		const bool shadowsEnabled = shadowSelection.type != ShadowLightType::None;
		const float shadowStrength = shadowSelection.strength;

		Layout::RendererLightingConstants lighting = {};
		lighting.cameraPosition = Math::float4(config.cameraPosition.x, config.cameraPosition.y, config.cameraPosition.z, shadowsEnabled ? shadowStrength : 0.0f);
		lighting.directionalLightDirection = Math::float4(lightDirection.x, lightDirection.y, lightDirection.z, shadowsEnabled ? 1.0f : 0.0f);
		lighting.directionalLightColor = Math::float4(lightColor.x, lightColor.y, lightColor.z, lightIntensity);
		lighting.ambientColor = Math::float4(
			config.ambientColor.x * config.environment.diffuseColor.x,
			config.ambientColor.y * config.environment.diffuseColor.y,
			config.ambientColor.z * config.environment.diffuseColor.z,
			config.ambientIntensity * config.environment.diffuseIntensity);
		lighting.shadowParams = Math::float4(config.shadowDepthBias, config.shadowSlopeBias, config.shadowNormalBias, static_cast<float>(config.shadowPcfRadius));
		lighting.pbrParams = Math::float4(
			config.pbr.minRoughness,
			config.pbr.ambientSpecularStrength,
			config.enableHdrRendering || RHI::IsSrgbFormat(config.renderTargetFormat) ? 0.0f : 1.0f,
			config.enableHdrRendering ? 0.0f : 1.0f);
		lighting.environmentColor = Math::float4(
			config.environment.specularColor.x,
			config.environment.specularColor.y,
			config.environment.specularColor.z,
			config.environment.specularIntensity);
		if(point != nullptr)
		{
			lighting.pointLightPositionRange = Math::float4(point->position.x, point->position.y, point->position.z, point->range);
			lighting.pointLightColorIntensity = Math::float4(point->color.x, point->color.y, point->color.z, point->intensity);
		}

		const bool needsFallbackDirectional = !hasAnySceneLight;
		const uint32_t directionalCount = directional != nullptr
			? static_cast<uint32_t>(directionalIndices.size())
			: (needsFallbackDirectional ? 1u : 0u);
		if(needsFallbackDirectional)
		{
			const Math::float3 direction = LightingGpuDetail::NormalizeDirection(config.directionalLightDirection);
			lighting.directionalLights[0] = {
				Math::float4(direction.x, direction.y, direction.z, std::max(config.directionalLightIntensity, 0.0f)),
				Math::float4(config.directionalLightColor.x, config.directionalLightColor.y, config.directionalLightColor.z, 0.0f)
			};
		}
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
		lighting.shadowLight = Math::float4(
			static_cast<float>(shadowSelection.type),
			static_cast<float>(shadowSelection.packedIndex),
			shadowSelection.strength,
			shadowsEnabled ? 1.0f : 0.0f);
		if(outShadowSelection != nullptr) *outShadowSelection = shadowSelection;
		return lighting;
	}

	[[nodiscard]] inline RendererShaderLayout::RendererLightingConstants BuildRendererLightingConstants(
		const Scene& scene,
		const RendererDesc& config)
	{
		return BuildRendererLightingConstants(scene, config, config.enableShadows, nullptr);
	}
}
