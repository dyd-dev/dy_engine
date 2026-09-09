#pragma once
#include <cstdint>
#include "ResourceState.h"

namespace dy::RHI
{
	// Buffer binding usages (Bitmask)
	enum class BufferUsage : uint32_t {
		None		= 0,
		Vertex		= 1 << 0,
		Index		= 1 << 1,
		Constant	= 1 << 2,
		Storage		= 1 << 3,
		Indirect	= 1 << 4
	};

	inline constexpr BufferUsage operator|(BufferUsage left, BufferUsage right)
	{
		return static_cast<BufferUsage>(static_cast<uint32_t>(left) | static_cast<uint32_t>(right));
	}

	inline constexpr BufferUsage operator&(BufferUsage left, BufferUsage right)
	{
		return static_cast<BufferUsage>(static_cast<uint32_t>(left) & static_cast<uint32_t>(right));
	}

	inline constexpr BufferUsage& operator|=(BufferUsage& left, BufferUsage right)
	{
		left = left | right;
		return left;
	}

	inline constexpr BufferUsage& operator&=(BufferUsage& left, BufferUsage right)
	{
		left = left & right;
		return left;
	}

	// Descriptor for creating a hardware buffer
	struct BufferDesc {
		uint32_t size = 0;
		uint32_t stride = 0;
		BufferUsage usage = {};
		ResourceState initialState = ResourceState::Undefined;
	};

	class Buffer
	{
	public:
		[[nodiscard]] const BufferDesc& GetDesc() const { return m_desc; }

	protected:
		virtual ~Buffer() = default;
		explicit Buffer(const BufferDesc& desc) : m_desc(desc) {}

	private:
		BufferDesc m_desc = {};
	};
}
