#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "RHI/IBuffer.h"
#include "RHI/IDevice.h"

namespace dy::Graphics
{
	class PerFrameBufferSet
	{
	public:
		[[nodiscard]] bool Initialize(
			RHI::IDevice* device,
			uint32_t frameCount,
			const RHI::BufferDesc& desc)
		{
			if(device == nullptr || desc.size == 0u) return false;
			Shutdown(device);

			const uint32_t resolvedFrameCount = std::max(frameCount, 1u);
			m_buffers.reserve(resolvedFrameCount);
			for(uint32_t frameIndex = 0; frameIndex < resolvedFrameCount; ++frameIndex)
			{
				RHI::IBuffer* buffer = device->CreateBuffer(desc);
				if(buffer == nullptr)
				{
					Shutdown(device);
					return false;
				}
				m_buffers.push_back(buffer);
			}
			return true;
		}

		void Shutdown(RHI::IDevice* device)
		{
			if(device != nullptr)
			{
				for(RHI::IBuffer* buffer : m_buffers)
				{
					if(buffer != nullptr) device->DestroyBuffer(buffer);
				}
			}
			m_buffers.clear();
		}

		[[nodiscard]] RHI::IBuffer* Get(uint32_t frameIndex) const
		{
			if(m_buffers.empty()) return nullptr;
			return m_buffers[frameIndex % static_cast<uint32_t>(m_buffers.size())];
		}

		[[nodiscard]] uint32_t GetFrameCount() const
		{
			return static_cast<uint32_t>(m_buffers.size());
		}

	private:
		std::vector<RHI::IBuffer*> m_buffers;
	};
}
