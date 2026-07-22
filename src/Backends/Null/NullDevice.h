#pragma once

#include "RHI/IDevice.h"

namespace dy::Backends
{
	class NullDevice final : public RHI::IDevice
	{
	public:
		NullDevice();
		~NullDevice() override;

		bool BeginFrame() override;
		uint32_t GetCurrentFrameIndex() const override;
		RHI::ICommandList* AcquireCommandList() override;

		void Submit(RHI::ICommandList** cmdLists, uint32_t count) override;
		void Present() override;

		RHI::IBuffer* CreateBuffer(const RHI::BufferDesc& desc) override;
		RHI::IShader* CreateShader(const RHI::ShaderDesc& desc) override;
		RHI::ISampler* CreateSampler(const RHI::SamplerDesc& desc) override;
		RHI::ITexture* CreateTexture(const RHI::TextureDesc& desc) override;
		bool UpdateTexture(RHI::ITexture* texture, const void* data, uint32_t rowPitch, RHI::ResourceState finalState) override;
		RHI::IPipelineState* CreateGraphicsPipeline(const RHI::GraphicsPipelineDesc& desc) override;
		RHI::IResourceSet* CreateResourceSet(RHI::IPipelineState* pipeline) override;
		bool UpdateResourceSet(RHI::IResourceSet* resourceSet, const RHI::ResourceSetWrite* writes, uint32_t writeCount) override;

		void DestroyBuffer(RHI::IBuffer* buffer) override;
		void DestroyShader(RHI::IShader* shader) override;
		void DestroySampler(RHI::ISampler* sampler) override;
		void DestroyTexture(RHI::ITexture* texture) override;
		void DestroyPipelineState(RHI::IPipelineState* pipeline) override;
		void DestroyResourceSet(RHI::IResourceSet* resourceSet) override;
		RHI::ITexture* GetBackBuffer() override;

	protected:
		int Initialize(const void* windowHandle, const RHI::DeviceDesc& desc) override;

	private:
		struct Impl;
		Impl* m_impl = nullptr;
	};
}
