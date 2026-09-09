#include "Graphics/Private/StockShaderAssets.h"
#if !defined(DY_NO_NATIVE_SHADERS)
#include "StockShaderBundle.h"
#include "CanvasShaderBundle.h"
#endif

namespace dy::Graphics
{
	ShaderAssets GetMeshShaderAssets(bool shadowsEnabled)
	{
#if defined(DY_NO_NATIVE_SHADERS)
		(void)shadowsEnabled;
		return {};
#else
		return shadowsEnabled ? Private::generated::withShadows : Private::generated::withoutShadows;
#endif
	}

	ShaderAssets GetCanvasShaderAssets()
	{
#if defined(DY_NO_NATIVE_SHADERS)
		return {};
#else
		return Private::generated::canvas;
#endif
	}
}

namespace dy::Graphics::Private
{
	StockShaderAssets GetStockShaderAssets(bool shadowsEnabled)
	{
		return GetMeshShaderAssets(shadowsEnabled);
	}
}
