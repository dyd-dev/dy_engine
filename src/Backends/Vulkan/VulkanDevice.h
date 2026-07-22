#pragma once
#include "RHI/IDevice.h"
#include <memory>

namespace dy::Backends
{

class VulkanDevice : public dy::RHI::IDevice
{
public:
	struct Impl;

	VulkanDevice();
	~VulkanDevice() override;

	bool BeginFrame() override;
	uint32_t GetCurrentFrameIndex() const override;
	dy::RHI::ICommandList* AcquireCommandList() override;
	void Submit(dy::RHI::ICommandList** cmdLists, uint32_t count) override;
	void Present() override;

	dy::RHI::IBuffer* CreateBuffer(const dy::RHI::BufferDesc& desc) override;
	dy::RHI::IShader* CreateShader(const dy::RHI::ShaderDesc& desc) override;
	dy::RHI::ISampler* CreateSampler(const dy::RHI::SamplerDesc& desc) override;
	dy::RHI::ITexture* CreateTexture(const dy::RHI::TextureDesc& desc) override;
	bool UpdateTexture(dy::RHI::ITexture* texture, const void* rgba8Pixels, uint32_t rowPitch) override;
	dy::RHI::IPipelineState* CreateGraphicsPipeline(const dy::RHI::GraphicsPipelineDesc& desc) override;
	dy::RHI::IResourceSet* CreateResourceSet(dy::RHI::IPipelineState* pipeline) override;
	bool UpdateResourceSet(dy::RHI::IResourceSet* resourceSet, const dy::RHI::ResourceSetWrite* writes, uint32_t writeCount) override;

	void DestroyBuffer(dy::RHI::IBuffer* buffer) override;
	void DestroyShader(dy::RHI::IShader* shader) override;
	void DestroySampler(dy::RHI::ISampler* sampler) override;
	void DestroyTexture(dy::RHI::ITexture* texture) override;
	void DestroyPipelineState(dy::RHI::IPipelineState* pipeline) override;
	void DestroyResourceSet(dy::RHI::IResourceSet* resourceSet) override;

	[[nodiscard]] dy::RHI::ITexture* GetBackBuffer() override;
	
protected:
	int Initialize(const void* windowHandle, const dy::RHI::DeviceDesc& desc) override;

private:
	std::unique_ptr<Impl> m_impl;
};

}
