#pragma once
#include <cstdint>

#include "RHI/RendererDefaults.h"

namespace dy::RHI
{
	class ICommandList;

	class IBuffer;
	class ITexture;
	class IPipelineState;

	struct BufferDesc;
	struct TextureDesc;
	struct GraphicsPipelineDesc;

	using DescriptorIndex = uint32_t;
	constexpr DescriptorIndex INVALID_DESCRIPTOR_INDEX = 0xFFFFFFFF;

	struct RendererShaderPaths
	{
		const char* vertexShaderPath = nullptr;
		const char* pixelShaderPath = nullptr;
		const char* shadowVertexShaderPath = nullptr;
	};

	struct DeviceDesc
	{
		RendererShaderPaths rendererShaderPaths = {};
		const char* fallbackTexturePath = nullptr;
		uint32_t maxFramesInFlight = RendererDefaults::kMaxFramesInFlight;
		uint32_t maxDrawsPerFrame = RendererDefaults::kMaxDrawsPerFrame;
		uint32_t defaultShadowMapResolution = RendererDefaults::kShadowMapResolution;
		uint32_t fallbackTextureWidth = RendererDefaults::kFallbackTextureWidth;
		uint32_t fallbackTextureHeight = RendererDefaults::kFallbackTextureHeight;
		uint64_t frameAcquireTimeoutNanoseconds = RendererDefaults::kFrameAcquireTimeoutNanoseconds;
	};

	class IDevice
	{
	public:
		virtual ~IDevice() = default;
		static IDevice* Create(const void* windowHandle, const DeviceDesc& desc = {});
		virtual void BeginFrame() = 0;
		virtual uint32_t GetCurrentFrameIndex() const = 0;
		virtual ICommandList* AcquireCommandList() = 0;
		virtual void Submit(ICommandList** cmdLists, uint32_t count) = 0;
		virtual void Present() = 0;
		virtual IBuffer* CreateBuffer(const BufferDesc& desc) = 0;
		virtual ITexture* CreateTexture(const TextureDesc& desc) = 0;
		virtual bool UpdateTexture(ITexture* texture, const void* rgba8Pixels, uint32_t rowPitch) = 0;
		virtual IPipelineState* CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) = 0;
		[[nodiscard]] virtual DescriptorIndex AllocateDescriptorSlot() = 0;
		virtual void UpdateDescriptorSlot(DescriptorIndex index, ITexture* texture) = 0;
		virtual void DestroyBuffer(IBuffer* buffer) = 0;
		virtual void DestroyTexture(ITexture* texture) = 0;
		virtual void DestroyPipelineState(IPipelineState* pipeline) = 0;
		virtual ITexture* GetBackBuffer() = 0;
		[[nodiscard]] virtual RendererShaderPaths GetDefaultRendererShaderPaths() const { return {}; }
		
	protected:
		virtual int Initialize(const void* windowHandle, const DeviceDesc& desc) = 0;
	};
}
