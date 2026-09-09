#pragma once

#include "Graphics/Shaders.h"

namespace dy::Graphics::Private
{
	using ShaderAsset = dy::Graphics::ShaderAsset;
	using StockShaderAssets = dy::Graphics::ShaderAssets;

	[[nodiscard]] StockShaderAssets GetStockShaderAssets(bool shadowsEnabled);
}
