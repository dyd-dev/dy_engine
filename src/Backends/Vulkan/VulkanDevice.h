#pragma once

#include "RHI/IDevice.h"

#include <memory>

namespace dy::Backends
{
	class VulkanDevice final : public dy::RHI::IDevice
	{
	public:
		struct Impl;

		VulkanDevice();
		~VulkanDevice() override;

		[[nodiscard]] bool CreateSwapchain(const dy::RHI::SwapchainDesc& desc) override;
		[[nodiscard]] bool BeginFrame() override;
		[[nodiscard]] dy::RHI::ICommandList* AcquireCommandList() override;
		[[nodiscard]] bool Submit(dy::RHI::ICommandList** commandLists, uint32_t count) override;
		void Present() override;

		[[nodiscard]] dy::RHI::TextureHandle GetBackBuffer() override;
		bool ReadTexture(dy::RHI::TextureHandle texture, dy::RHI::TextureReadback& result) override;

		[[nodiscard]] dy::RHI::BufferHandle CreateBuffer(const dy::RHI::BufferDesc& desc) override;
		[[nodiscard]] dy::RHI::TextureHandle CreateTexture(const dy::RHI::TextureDesc& desc) override;
		[[nodiscard]] dy::RHI::ShaderHandle CreateShader(const dy::RHI::ShaderDesc& desc) override;
		[[nodiscard]] dy::RHI::PipelineHandle CreateGraphicsPipeline(const dy::RHI::GraphicsPipelineDesc& desc) override;
		[[nodiscard]] dy::RHI::ResourceSetHandle CreateResourceSet(const dy::RHI::ResourceSetDesc& desc) override;

		void DestroyBuffer(dy::RHI::BufferHandle buffer) override;
		void DestroyTexture(dy::RHI::TextureHandle texture) override;
		void DestroyShader(dy::RHI::ShaderHandle shader) override;
		void DestroyPipeline(dy::RHI::PipelineHandle pipeline) override;
		void DestroyResourceSet(dy::RHI::ResourceSetHandle resourceSet) override;

		bool UpdateBuffer(dy::RHI::ICommandList& commandList, dy::RHI::BufferHandle buffer, uint32_t offset, const void* data, uint32_t size) override;
		bool UpdateTexture(
			dy::RHI::ICommandList& commandList,
			dy::RHI::TextureHandle texture,
			uint32_t mipLevel,
			uint32_t arrayLayer,
			const void* data,
			uint32_t dataSize,
			uint32_t rowPitch,
			uint32_t slicePitch) override;

	protected:
		int Initialize(const void* windowHandle, const dy::RHI::DeviceDesc& desc) override;

	private:
		std::unique_ptr<Impl> m_impl;
	};
}
