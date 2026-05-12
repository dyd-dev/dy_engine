#pragma once
/* Buffer
*
* Vertex, Index, Constant, Storage Buffer 등을 위한 버퍼 객체입니다. GPU 메모리에 데이터를 저장하고, 렌더링 과정에서 이를 참조할 수 있도록 합니다.
* GPU 메모리에 데이터를 업로드하고, 필요한 경우 CPU에서 데이터를 읽거나 쓸 수 있도록 하는 기능을 제공합니다. 또한, 버퍼의 크기, 형식, 사용 용도 등을 정의할 수 있습니다.
*
* 멀티스레딩 환경을 위해서 Buffer는 고속 매핑 방식을 지원해야 하며,
* Backend 내부에서는 버퍼를 생성할 때 CPU_TO_GPU 메모리 힙(Upload Heap)으로 생성해야 병목이 생기지 않습니다.
*/
#include <cstdint>
#include "Enums.h"

namespace dy::RHI
{
	// Descriptor for creating a hardware buffer
	struct BufferDesc {
		uint32_t size;
		uint32_t stride;
		BufferUsage usage;
	};

	class IBuffer
	{
	public:
		virtual ~IBuffer() = default;

		// Maps the GPU memory to CPU address space
		// Map a specific range to thread contention
		virtual void* Map(uint32_t offset, uint32_t size) = 0;
		virtual void Unmap() = 0;

		virtual uint32_t GetSize() const = 0;
		virtual uint32_t GetStride() const = 0;
	};
}