#pragma once

#include "Core/Types.h"

#include <string>

namespace dy::Graphics
{
	struct MaterialTexturePaths
	{
		std::string baseColorTexture;
		std::string metallicRoughnessTexture;
		std::string normalTexture;
		std::string occlusionTexture;
		std::string emissiveTexture;

		[[nodiscard]] bool HasAny() const
		{
			return !baseColorTexture.empty()
				|| !metallicRoughnessTexture.empty()
				|| !normalTexture.empty()
				|| !occlusionTexture.empty()
				|| !emissiveTexture.empty();
		}
	};

	struct MaterialImportData
	{
		MaterialTexturePaths texturePaths;
		Material material;
		bool hasBaseColor = false;
		bool hasEmissiveColor = false;
		bool hasMetallicFactor = false;
		bool hasRoughnessFactor = false;
		bool hasNormalScale = false;
		bool hasOcclusionStrength = false;

		[[nodiscard]] bool HasMaterialValues() const
		{
			return hasBaseColor
				|| hasEmissiveColor
				|| hasMetallicFactor
				|| hasRoughnessFactor
				|| hasNormalScale
				|| hasOcclusionStrength;
		}

		[[nodiscard]] bool HasAny() const
		{
			return texturePaths.HasAny() || HasMaterialValues();
		}

		void ApplyTo(Material& target) const
		{
			if (hasBaseColor) target.baseColor = material.baseColor;
			if (hasEmissiveColor) target.emissiveColor = material.emissiveColor;
			if (hasMetallicFactor) target.metallicFactor = material.metallicFactor;
			if (hasRoughnessFactor) target.roughnessFactor = material.roughnessFactor;
			if (hasNormalScale) target.normalScale = material.normalScale;
			if (hasOcclusionStrength) target.occlusionStrength = material.occlusionStrength;
		}
	};
}
