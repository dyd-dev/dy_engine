#pragma once
#include <cstdint>

#include "Math/Math.h"
#include "Types.h"

namespace dy::Graphics
{
	enum MaterialTextureFlags : uint32_t
	{
		MaterialTexture_BaseColor = 1u << 0,
		MaterialTexture_MetallicRoughness = 1u << 1,
		MaterialTexture_Normal = 1u << 2,
		MaterialTexture_Occlusion = 1u << 3,
		MaterialTexture_Emissive = 1u << 4
	};

	struct MaterialData
	{
		Math::float4 baseColor = {1.0f, 1.0f, 1.0f, 1.0f};
		TextureID baseColorTex = TextureID::Invalid;
		TextureID metallicRoughnessTex = TextureID::Invalid;
		TextureID normalTex = TextureID::Invalid;
		TextureID occlusionTex = TextureID::Invalid;
		TextureID emissiveTex = TextureID::Invalid;
		Math::float3 emissiveColor = Math::float3(0.0f, 0.0f, 0.0f);
		float metallic = 0.0f;
		float roughness = 0.5f;
		float normalScale = 1.0f;
		float occlusionStrength = 1.0f;
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
