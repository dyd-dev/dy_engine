#pragma once
#include "dyf/Types.h"
#include <cstdint>
#include "dyf/Image.h"

namespace dyf
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
		// 기본 3D 셰이더는 RGB만 사용한다. 불투명도에는 baseColor.w만 반영한다.
		Image baseColorTexture = {};
		Math::float3 emissiveColor = Math::float3(0.0f, 0.0f, 0.0f);
		float metallicFactor = 0.0f;
		float roughnessFactor = 0.5f;
		float normalScale = 1.0f;
		float occlusionStrength = 1.0f;
		Image metallicRoughnessTexture = {};
		Image normalTexture = {};
		Image occlusionTexture = {};
		Image emissiveTexture = {};
	};
}
