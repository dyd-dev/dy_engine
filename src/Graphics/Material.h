#pragma once
#include <cstdint>

#include "Math/Math.h"
#include "Types.h"

namespace dy::Graphics
{
	struct MaterialData
	{
		Math::float4 baseColor = {1.0f, 1.0f, 1.0f, 1.0f};
		TextureID baseColorTex = TextureID::Invalid;
		float metallic = 0.0f;
		float roughness = 1.0f;
	};

	struct alignas(16) MaterialDrawConstants
	{
		Math::float4x4 worldMatrix = Math::float4x4::Identity();
		Math::float4 baseColor = {1.0f, 1.0f, 1.0f, 1.0f};
		uint32_t baseColorTextureIndex = ToIndex(TextureID::Invalid);
		float metallic = 0.0f;
		float roughness = 1.0f;
		uint32_t materialFlags = 0;
	};

	static_assert(sizeof(MaterialDrawConstants) == 96, "MaterialDrawConstants must match backend root constant layout.");
}
