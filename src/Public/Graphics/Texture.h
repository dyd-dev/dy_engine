#pragma once
#include "Core/Types.h"
#include <string>
#include <vector>

namespace dy::Graphics
{
	struct TextureAsset
	{
		std::string sourcePath;
		uint32_t width = 0;
		uint32_t height = 0;
		std::vector<uint8_t> rgba8 = {};
	};
	[[nodiscard]] bool LoadImage(const std::string& path, TextureAsset& image);
}
