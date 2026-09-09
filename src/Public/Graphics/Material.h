#pragma once
#include "Core/Types.h"
#include "Graphics/Texture.h"
#include <cstdint>

namespace dy::Graphics
{
	enum class MaterialTextureKind : uint32_t
	{
		BaseColor = 0,
		MetallicRoughness,
		Normal,
		Occlusion,
		Emissive,
		Count
	};

	inline constexpr uint32_t kMaterialTextureCount = static_cast<uint32_t>(MaterialTextureKind::Count);

	struct MaterialDesc
	{
		Math::float4 baseColor = Math::float4(1.0f, 1.0f, 1.0f, 1.0f);
		TextureID baseColorTexture = TextureID::Invalid;
		Math::float3 emissiveColor = Math::float3(0.0f, 0.0f, 0.0f);
		float metallicFactor = 0.0f;
		float roughnessFactor = 0.5f;
		float normalScale = 1.0f;
		float occlusionStrength = 1.0f;
		TextureID metallicRoughnessTexture = TextureID::Invalid;
		TextureID normalTexture = TextureID::Invalid;
		TextureID occlusionTexture = TextureID::Invalid;
		TextureID emissiveTexture = TextureID::Invalid;
	};
}
