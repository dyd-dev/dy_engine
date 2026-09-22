#pragma once

#include "dyf/RHI/IDevice.h"

namespace dyf::Backends
{
	class NullDevice : public RHI::IDevice
	{
	public:
		NullDevice();
		~NullDevice() override;

		void DestroySwapchainNative() override;
        bool WaitIdleNative() override;
        bool IsLostNative() const override;
        bool SupportsNative(RHI::Feature) const override;
        uint64_t GetLimitNative(RHI::Limit) const override;
        bool SupportsSamplerNative(const RHI::SamplerDesc&) const override;
        bool SupportsPipelineLayoutNative(const RHI::PipelineLayoutDesc&) const override;
        bool SupportsGraphicsPipelineNative(const RHI::GraphicsPipelineDesc&) const override;
        bool SupportsMeshPipelineNative(const RHI::MeshPipelineDesc&) const override;
        uint64_t GetCompletedSubmissionNative() override;
        uint64_t GetLastSubmissionNative() const override;

		bool CreateSwapchainNative(const RHI::SwapchainDesc& desc) override;
		bool BeginFrameNative() override;
		RHI::ICommandList* AcquireCommandListNative() override;
        void DiscardCommandListNative(RHI::ICommandList*) override;

		bool SubmitNative(RHI::ICommandList** cmdLists, uint32_t count) override;
		bool PresentNative() override;

		RHI::BufferHandle CreateBufferNative(const RHI::BufferDesc& desc) override;
		RHI::TextureHandle CreateTextureNative(const RHI::TextureDesc& desc) override;
		RHI::ShaderHandle CreateShaderNative(const RHI::ShaderDesc& desc) override;
		RHI::PipelineHandle CreateGraphicsPipelineNative(const RHI::GraphicsPipelineDesc& desc) override;
		RHI::PipelineHandle CreateMeshPipelineNative(const RHI::MeshPipelineDesc& desc) override;
		RHI::ResourceSetHandle CreateResourceSetNative(const RHI::ResourceSetDesc& desc) override;

		void DestroyBufferNative(RHI::BufferHandle buffer) override;
		void DestroyTextureNative(RHI::TextureHandle texture) override;
		void DestroyShaderNative(RHI::ShaderHandle shader) override;
		void DestroyPipelineNative(RHI::PipelineHandle pipeline) override;
		void DestroyResourceSetNative(RHI::ResourceSetHandle resourceSet) override;

		bool UpdateBufferNative(
			RHI::ICommandList& commandList,
			RHI::BufferHandle buffer,
			uint32_t offset,
			const void* data,
			uint32_t size) override;
		bool UpdateTextureNative(
			RHI::ICommandList& commandList,
			RHI::TextureHandle texture,
			uint32_t mipLevel,
			uint32_t arrayLayer,
			const void* data,
			uint32_t dataSize,
			uint32_t rowPitch,
			uint32_t slicePitch) override;
		RHI::TextureHandle GetBackBufferNative() override;
		bool ReadTextureNative(RHI::TextureHandle, RHI::TextureReadback&) override { return false; }

	protected:
		int Initialize(const void* windowHandle, const RHI::DeviceDesc& desc) override;

	private:
		struct Impl;
		Impl* m_impl = nullptr;
	};
}
