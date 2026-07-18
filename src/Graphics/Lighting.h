#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

#include "Math/Math.h"

namespace dy::Graphics
{
	struct LightingLimits
	{
		static constexpr uint32_t MaxDirectionalLights = 4u;
		static constexpr uint32_t MaxPointLights = 16u;
		static constexpr uint32_t MaxSpotLights = 16u;
		static constexpr uint32_t MaxRectAreaLights = 4u;
		static constexpr uint32_t MaxDiscAreaLights = 4u;
	};

	struct LightControl
	{
		bool enabled = true;
		bool castShadow = false;
		int32_t priority = 0;
		float shadowStrength = 1.0f;
	};

	struct DirectionalLight : LightControl
	{
		DirectionalLight()
		{
			castShadow = true;
			shadowStrength = 0.45f;
		}

		Math::float3 direction = Math::float3(0.35f, 0.65f, 0.68f);
		Math::float3 color = Math::float3(1.0f, 0.94f, 0.82f);
		float illuminanceLux = 4.0f;
		float angularRadiusRadians = 0.00465f;
	};

	struct PointLight : LightControl
	{
		PointLight()
		{
			castShadow = true;
			shadowStrength = 0.5f;
		}

		Math::float3 position = Math::float3(0.0f, 0.0f, 2.0f);
		Math::float3 color = Math::float3(1.0f, 0.94f, 0.82f);
		float luminousIntensityCandela = 6.0f;
		float rangeMeters = 6.0f;
		float sourceRadiusMeters = 0.05f;
	};

	struct SpotLight : LightControl
	{
		Math::float3 position = Math::float3(0.0f, 0.0f, 2.0f);
		Math::float3 direction = Math::float3(0.0f, 0.0f, -1.0f);
		Math::float3 color = Math::float3(1.0f, 0.94f, 0.82f);
		float luminousIntensityCandela = 6.0f;
		float rangeMeters = 6.0f;
		float innerConeRadians = 0.34906585f;
		float outerConeRadians = 0.52359878f;
		float sourceRadiusMeters = 0.05f;
	};

	struct RectAreaLight : LightControl
	{
		Math::float3 position = Math::float3(0.0f, 0.0f, 2.0f);
		Math::float3 direction = Math::float3(0.0f, 0.0f, -1.0f);
		Math::float3 up = Math::float3(0.0f, 1.0f, 0.0f);
		Math::float3 color = Math::float3(1.0f, 0.94f, 0.82f);
		float luminanceNits = 100.0f;
		float widthMeters = 1.0f;
		float heightMeters = 1.0f;
	};

	struct DiscAreaLight : LightControl
	{
		Math::float3 position = Math::float3(0.0f, 0.0f, 2.0f);
		Math::float3 direction = Math::float3(0.0f, 0.0f, -1.0f);
		Math::float3 up = Math::float3(0.0f, 1.0f, 0.0f);
		Math::float3 color = Math::float3(1.0f, 0.94f, 0.82f);
		float luminanceNits = 100.0f;
		float radiusMeters = 0.5f;
	};

	struct EnvironmentLight
	{
		std::string hdrPath;
		float intensityMultiplier = 1.0f;
		float rotationRadians = 0.0f;
	};

	[[nodiscard]] inline float ComputePunctualAttenuation(float distanceMeters, float rangeMeters)
	{
		if(distanceMeters < 0.0f || rangeMeters <= 0.0f || distanceMeters >= rangeMeters) return 0.0f;
		const float safeDistanceSquared = std::max(distanceMeters * distanceMeters, 0.0001f);
		const float distanceRatio = distanceMeters / rangeMeters;
		const float ratioSquared = distanceRatio * distanceRatio;
		const float window = std::max(1.0f - ratioSquared * ratioSquared, 0.0f);
		return (window * window) / safeDistanceSquared;
	}

	[[nodiscard]] inline float ComputeSpotConeAttenuation(float cosTheta, float innerCos, float outerCos)
	{
		if(innerCos <= outerCos) return cosTheta >= innerCos ? 1.0f : 0.0f;
		const float t = std::clamp((cosTheta - outerCos) / (innerCos - outerCos), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	template <typename LightType>
	[[nodiscard]] std::vector<uint32_t> SelectActiveLightIndices(
		const std::vector<LightType>& lights,
		uint32_t capacity)
	{
		std::vector<uint32_t> indices;
		indices.reserve(lights.size());
		for(uint32_t index = 0; index < static_cast<uint32_t>(lights.size()); ++index)
		{
			if(lights[index].enabled) indices.push_back(index);
		}
		std::stable_sort(indices.begin(), indices.end(), [&lights](uint32_t left, uint32_t right)
		{
			return lights[left].priority > lights[right].priority;
		});
		if(indices.size() > capacity) indices.resize(capacity);
		return indices;
	}
}
