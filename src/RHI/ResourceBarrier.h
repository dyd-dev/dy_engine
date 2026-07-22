#pragma once

#include <cstdint>

namespace dy::RHI
{
	class ITexture;

	enum class ResourceState : uint8_t
	{
		Undefined,
		ShaderResource,
		RenderTarget,
		DepthStencilWrite,
		Storage,
		Present
	};

	struct TextureBarrier
	{
		ITexture* texture = nullptr;
		ResourceState before = ResourceState::Undefined;
		ResourceState after = ResourceState::Undefined;
	};
}
