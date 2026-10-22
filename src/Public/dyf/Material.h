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

	enum class MaterialAlphaMode : uint8_t { Opaque, Mask, Blend };

	struct MaterialDesc
	{
		Math::float4 baseColor = Math::float4(1.0f, 1.0f, 1.0f, 1.0f);
		// Base-color texture alpha multiplies baseColor.w in Mask and Blend modes.
		Image baseColorTexture = {};
		Math::float3 emissiveColor = Math::float3(0.0f, 0.0f, 0.0f);
		float metallicFactor = 0.0f;
		float roughnessFactor = 0.5f;
		float normalScale = 1.0f;
		// +1 for OpenGL/glTF tangent normals, -1 for DirectX tangent normals.
		float normalYSign = 1.0f;
		float occlusionStrength = 1.0f;
		// Raster transmission uses the opaque scene behind the surface and HDRI.
		float transmissionFactor = 0.0f;
		float indexOfRefraction = 1.5f;
		float thickness = 0.0f;
		Math::float3 attenuationColor = Math::float3(1.0f, 1.0f, 1.0f);
		// Zero disables volume absorption; lengths use scene world units.
		float attenuationDistance = 0.0f;
		Image metallicRoughnessTexture = {};
		Image normalTexture = {};
		Image occlusionTexture = {};
		Image emissiveTexture = {};
		MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
		float alphaCutoff = 0.5f;
		bool doubleSided = false;
		// 조명·발광 대신 기본색과 알파를 표시한다. 공통 출력 색변환은 그대로 적용한다.
		bool unlit = false;
	};

	// Optical transmission and geometric alpha coverage are independent. Both
	// need the transparent lighting path, while retaining their authored alpha mode.
	inline bool UsesBlendedRendering(const MaterialDesc& material)
	{
		return material.alphaMode == MaterialAlphaMode::Blend || material.transmissionFactor > 0.0f;
	}
}
