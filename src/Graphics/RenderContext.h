#pragma once
#include <cstdint>

namespace dy::RHI
{
	class IDevice;
	class ITexture;
}

namespace dy::Graphics
{
	struct RendererResources;

	struct RenderContext
	{
		RHI::IDevice* device = nullptr;
		RHI::ITexture* backBuffer = nullptr;
		uint32_t frameIndex = 0;
		RendererResources* resources = nullptr;
	};
}
