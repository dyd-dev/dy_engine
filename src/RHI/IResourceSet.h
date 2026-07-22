#pragma once

#include <cstdint>

namespace dy::RHI
{
	class IBuffer;
	class ISampler;
	class ITexture;

	struct ResourceSetWrite
	{
		uint32_t set = 0;
		uint32_t binding = 0;
		uint32_t arrayElement = 0;
		IBuffer* buffer = nullptr;
		ITexture* texture = nullptr;
		ISampler* sampler = nullptr;
		uint32_t bufferOffset = 0;
		uint32_t bufferSize = 0;
	};

	class IResourceSet
	{
	public:
		virtual ~IResourceSet() = default;
	};
}
