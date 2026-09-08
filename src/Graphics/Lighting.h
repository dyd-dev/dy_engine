#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "Math/Math.h"

namespace dy::Graphics
{
	struct DirectionalLight
	{
		bool enabled = true;
		int32_t priority = 0;
		Math::float3 direction = Math::float3(0.35f, 0.65f, 0.68f);
		Math::float3 color = Math::float3(1.0f, 0.94f, 0.82f);
		float intensity = 4.0f;
		bool castShadow = true;
		float shadowStrength = 0.45f;
	};

	struct PointLight
	{
		bool enabled = true;
		int32_t priority = 0;
		Math::float3 position = Math::float3(0.0f, 0.0f, 2.0f);
		float range = 6.0f;
		Math::float3 color = Math::float3(1.0f, 0.94f, 0.82f);
		float intensity = 6.0f;
		Math::float3 direction = Math::float3(0.0f, 0.0f, -1.0f);
		bool castShadow = true;
		float shadowStrength = 0.5f;
	};

	struct SpotLight
	{
		bool enabled = true;
		int32_t priority = 0;
		Math::float3 position = Math::float3(0.0f, 0.0f, 2.0f);
		float range = 6.0f;
		Math::float3 direction = Math::float3(0.0f, 0.0f, -1.0f);
		float outerConeRadians = 0.52359878f;
		Math::float3 color = Math::float3(1.0f, 0.94f, 0.82f);
		float intensity = 6.0f;
		float innerConeRadians = 0.34906585f;
		bool castShadow = false;
		float shadowStrength = 1.0f;
	};

	struct RectAreaLight
	{
		bool enabled = true;
		int32_t priority = 0;
		Math::float3 position = Math::float3(0.0f, 0.0f, 2.0f);
		float intensity = 100.0f;
		Math::float3 direction = Math::float3(0.0f, 0.0f, -1.0f);
		float width = 1.0f;
		Math::float3 up = Math::float3(0.0f, 1.0f, 0.0f);
		float height = 1.0f;
		Math::float3 color = Math::float3(1.0f, 0.94f, 0.82f);
	};

	struct DiscAreaLight
	{
		bool enabled = true;
		int32_t priority = 0;
		Math::float3 position = Math::float3(0.0f, 0.0f, 2.0f);
		float intensity = 100.0f;
		Math::float3 direction = Math::float3(0.0f, 0.0f, -1.0f);
		float radius = 0.5f;
		Math::float3 up = Math::float3(0.0f, 1.0f, 0.0f);
		Math::float3 color = Math::float3(1.0f, 0.94f, 0.82f);
	};

	template <typename LightType>
	[[nodiscard]] inline std::vector<uint32_t> SelectActiveLightIndices(
		const std::vector<LightType>& lights,
		uint32_t capacity)
	{
		std::vector<uint32_t> indices;
		indices.reserve(lights.size());
		for(uint32_t index = 0u; index < static_cast<uint32_t>(lights.size()); ++index)
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
