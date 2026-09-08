#pragma once
#include <cstdint>
#include "Format.h"

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
	DY_RHI_ENABLE_ENUM_FLAGS(BufferUsage)

	enum class BufferMemoryUsage : uint8_t {
		CpuToGpu,
		GpuOnly
	};

	// Descriptor for creating a hardware buffer
	struct BufferDesc {
		uint32_t size = 0;
		uint32_t stride = 0;
		BufferUsage usage = {};
		BufferMemoryUsage memoryUsage = BufferMemoryUsage::CpuToGpu;
	};

	class IBuffer
	{
	public:
		virtual ~IBuffer() = default;

		[[nodiscard]] const BufferDesc& GetDesc() const { return m_desc; }
		[[nodiscard]] uint32_t GetSize() const { return m_desc.size; }
		[[nodiscard]] uint32_t GetStride() const { return m_desc.stride; }
		[[nodiscard]] BufferUsage GetUsage() const { return m_desc.usage; }
		[[nodiscard]] BufferMemoryUsage GetMemoryUsage() const { return m_desc.memoryUsage; }

		virtual void* Map(uint32_t offset) = 0;
		virtual void Unmap() = 0;

	protected:
		explicit IBuffer(const BufferDesc& desc) : m_desc(desc) {}
		void SetDesc(const BufferDesc& desc) { m_desc = desc; }

	private:
		BufferDesc m_desc = {};
	};
}
