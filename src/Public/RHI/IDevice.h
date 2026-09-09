#pragma once

#include <cstdint>

#include "ResourceHandles.h"
#include "Swapchain.h"

namespace dy::RHI
{
	class ICommandList;
	struct BufferDesc;
	struct GraphicsPipelineDesc;
	struct ResourceSetDesc;
	struct ShaderDesc;
	struct TextureDesc;
	struct TextureReadback;

	struct DeviceDesc
	{
		uint32_t maxFramesInFlight = 2;
	};

	class IDevice
	{
	public:
		virtual ~IDevice() = default;

		[[nodiscard]] static IDevice* Create(const void* windowHandle, const DeviceDesc& desc = {});
		[[nodiscard]] const DeviceDesc& GetDesc() const { return m_desc; }
		[[nodiscard]] virtual bool CreateSwapchain(const SwapchainDesc& desc) = 0;

		[[nodiscard]] virtual bool BeginFrame() = 0;
		[[nodiscard]] virtual ICommandList* AcquireCommandList() = 0;

		// 유효한 owned·closed 목록은 성공 여부와 무관하게 소비한다.
		[[nodiscard]] virtual bool Submit(ICommandList** commandLists, uint32_t count) = 0;
		virtual void Present() = 0;

		// Swapchain 소유 handle이다. DestroyTexture에 전달하지 않는다.
		[[nodiscard]] virtual TextureHandle GetBackBuffer() = 0;

		// Synchronous diagnostic readback of mip 0 / layer 0 in RGBA8 or BGRA8.
		// Submit all recorded work first. Resource state is preserved. A swapchain
		// image must have allowReadback enabled and be read after Submit, before Present.
		// Returns false for unsupported formats, invalid use, or a non-rasterizing backend.
		[[nodiscard]] virtual bool ReadTexture(TextureHandle texture, TextureReadback& result) = 0;

		[[nodiscard]] virtual BufferHandle CreateBuffer(const BufferDesc& desc) = 0;
		[[nodiscard]] virtual TextureHandle CreateTexture(const TextureDesc& desc) = 0;
		[[nodiscard]] virtual ShaderHandle CreateShader(const ShaderDesc& desc) = 0;
		[[nodiscard]] virtual PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) = 0;
		[[nodiscard]] virtual ResourceSetHandle CreateResourceSet(const ResourceSetDesc& desc) = 0;

		virtual void DestroyBuffer(BufferHandle buffer) = 0;
		virtual void DestroyTexture(TextureHandle texture) = 0;
		virtual void DestroyShader(ShaderHandle shader) = 0;
		virtual void DestroyPipeline(PipelineHandle pipeline) = 0;
		virtual void DestroyResourceSet(ResourceSetHandle resourceSet) = 0;

		virtual bool UpdateBuffer(
			ICommandList& commandList,
			BufferHandle buffer,
			uint32_t offset,
			const void* data,
			uint32_t size) = 0;
		virtual bool UpdateTexture(
			ICommandList& commandList,
			TextureHandle texture,
			uint32_t mipLevel,
			uint32_t arrayLayer,
			const void* data,
			uint32_t dataSize,
			uint32_t rowPitch,
			uint32_t slicePitch) = 0;

	protected:
		virtual int Initialize(const void* windowHandle, const DeviceDesc& desc) = 0;

	private:
		void SetDesc(const DeviceDesc& desc) { m_desc = desc; }

		DeviceDesc m_desc = {};
	};
}
