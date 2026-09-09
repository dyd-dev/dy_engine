#pragma once

#include <cstdint>

namespace dy::RHI
{
	enum class ResourceState : uint8_t
	{
		Undefined,
		Common,
		CopyDestination,
		VertexBuffer,
		IndexBuffer,
		ConstantBuffer,
		ShaderResource,
		UnorderedAccess,
		RenderTarget,
		DepthRead,
		DepthWrite,
		Present
	};

}
