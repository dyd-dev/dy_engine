#pragma once

#include "ResourceHandles.h"
#include "ResourceState.h"
#include "Texture.h"

namespace dy::RHI
{
	struct ResourceBarrierDesc
	{
		BufferHandle buffer = nullptr;
		TextureHandle texture = nullptr;
		ResourceState before = ResourceState::Undefined;
		ResourceState after = ResourceState::Undefined;
		TextureSubresourceRange subresources = {};
	};
}
