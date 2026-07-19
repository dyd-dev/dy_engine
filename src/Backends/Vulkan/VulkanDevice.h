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
	dy::RHI::ITexture* CreateTexture(const dy::RHI::TextureDesc& desc) override;
	bool UpdateTexture(dy::RHI::ITexture* texture, const void* rgba8Pixels, uint32_t rowPitch) override;
	dy::RHI::IPipelineState* CreateGraphicsPipeline(const dy::RHI::GraphicsPipelineDesc& desc) override;
	[[nodiscard]] dy::RHI::DescriptorIndex AllocateDescriptorSlot() override;
	void UpdateDescriptorSlot(dy::RHI::DescriptorIndex index, dy::RHI::ITexture* texture) override;
	void UpdateDescriptorSlot(dy::RHI::DescriptorIndex, dy::RHI::IBuffer*) override {}

	void DestroyBuffer(dy::RHI::IBuffer* buffer) override;
	void DestroyShader(dy::RHI::IShader* shader) override;
	void DestroyTexture(dy::RHI::ITexture* texture) override;
	void DestroyPipelineState(dy::RHI::IPipelineState* pipeline) override;

	[[nodiscard]] dy::RHI::ITexture* GetBackBuffer() override;
	
protected:
	int Initialize(const void* windowHandle, const dy::RHI::DeviceDesc& desc) override;

private:
	std::unique_ptr<Impl> m_impl;
};

}
