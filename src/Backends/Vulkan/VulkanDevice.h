#pragma once

#include "dyf/RHI/IDevice.h"

#include <memory>

namespace dyf::Backends
{
	class VulkanDevice final : public dyf::RHI::IDevice
	{
	public:
		struct Impl;

		VulkanDevice();
		~VulkanDevice() override;

		void DestroySwapchainNative() override;
        bool WaitIdleNative() override;
        bool IsLostNative() const override;
        bool SupportsNative(RHI::Feature) const override;
        uint64_t GetLimitNative(RHI::Limit) const override;
        bool SupportsSamplerNative(const RHI::SamplerDesc&) const override;
        bool SupportsPipelineLayoutNative(const RHI::PipelineLayoutDesc&) const override;
        bool SupportsGraphicsPipelineNative(const RHI::GraphicsPipelineDesc&) const override;
        uint64_t GetCompletedSubmissionNative() override;
        uint64_t GetLastSubmissionNative() const override; bool CreateSwapchainNative(const dyf::RHI::SwapchainDesc& desc) override;
		[[nodiscard]] bool BeginFrameNative() override;
		[[nodiscard]] dyf::RHI::ICommandList* AcquireCommandListNative() override;
        void DiscardCommandListNative(RHI::ICommandList*) override;
		[[nodiscard]] bool SubmitNative(dyf::RHI::ICommandList** commandLists, uint32_t count) override;
		bool PresentNative() override;

		[[nodiscard]] dyf::RHI::TextureHandle GetBackBufferNative() override;
		bool ReadTextureNative(dyf::RHI::TextureHandle texture, dyf::RHI::TextureReadback& result) override;

		[[nodiscard]] dyf::RHI::BufferHandle CreateBufferNative(const dyf::RHI::BufferDesc& desc) override;
		[[nodiscard]] dyf::RHI::TextureHandle CreateTextureNative(const dyf::RHI::TextureDesc& desc) override;
		[[nodiscard]] dyf::RHI::ShaderHandle CreateShaderNative(const dyf::RHI::ShaderDesc& desc) override;
		[[nodiscard]] dyf::RHI::PipelineHandle CreateComputePipelineNative(const RHI::ComputePipelineDesc&) override;
        RHI::PipelineHandle CreateGraphicsPipelineNative(const dyf::RHI::GraphicsPipelineDesc& desc) override;
		[[nodiscard]] dyf::RHI::ResourceSetHandle CreateResourceSetNative(const dyf::RHI::ResourceSetDesc& desc) override;

		void DestroyBufferNative(dyf::RHI::BufferHandle buffer) override;
		void DestroyTextureNative(dyf::RHI::TextureHandle texture) override;
		void DestroyShaderNative(dyf::RHI::ShaderHandle shader) override;
		void DestroyPipelineNative(dyf::RHI::PipelineHandle pipeline) override;
		void DestroyResourceSetNative(dyf::RHI::ResourceSetHandle resourceSet) override;

		bool UpdateBufferNative(dyf::RHI::ICommandList& commandList, dyf::RHI::BufferHandle buffer, uint32_t offset, const void* data, uint32_t size) override;
		bool UpdateTextureNative(
			dyf::RHI::ICommandList& commandList,
			dyf::RHI::TextureHandle texture,
			uint32_t mipLevel,
			uint32_t arrayLayer,
			const void* data,
			uint32_t dataSize,
			uint32_t rowPitch,
			uint32_t slicePitch) override;

	protected:
		int Initialize(const void* windowHandle, const dyf::RHI::DeviceDesc& desc) override;

	private:
        RHI::TimestampQueryHandle CreateTimestampQueryNative(const RHI::TimestampQueryDesc&) override;
        void DestroyTimestampQueryNative(RHI::TimestampQueryHandle) override;
        bool ReadTimestampsNative(RHI::TimestampQueryHandle,uint32_t,uint32_t,uint64_t*) override;
        double GetTimestampPeriodNative() const override;
        uint32_t GetTimestampValidBitsNative() const override;
		std::unique_ptr<Impl> m_impl;
	};
}
