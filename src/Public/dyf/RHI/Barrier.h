#pragma once

#include "ResourceHandles.h"
#include "ResourceState.h"
#include "Texture.h"

namespace dyf::RHI
{
	struct ResourceBarrierDesc
	{
		// before는 실제 현재 상태와 정확히 일치해야 한다.
		// Undefined는 자원의 최초 상태이며 현재 상태를 무시하거나 내용을 폐기하는 wildcard가 아니다.
		BufferHandle buffer = nullptr;
		TextureHandle texture = nullptr;
		ResourceState before = ResourceState::Undefined;
		ResourceState after = ResourceState::Undefined;
		TextureSubresourceRange subresources = {};
	};
}
