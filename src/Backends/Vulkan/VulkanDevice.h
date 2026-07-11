#pragma once
#include "RHI/IDevice.h"
#include <memory>
#include <vector>

namespace dy::Backends
{

class VulkanDevice : public dy::RHI::IDevice
{
public:
	struct Impl;

	VulkanDevice();
	~VulkanDevice() override;

	void BeginFrame() override;
	uint32_t GetCurrentFrameIndex() const override;
	dy::RHI::ICommandList* AcquireCommandList() override;
	void Submit(dy::RHI::ICommandList** cmdLists, uint32_t count) override;
	void Present() override;

	dy::RHI::IBuffer* CreateBuffer(const dy::RHI::BufferDesc& desc) override;
	dy::RHI::ITexture* CreateTexture(const dy::RHI::TextureDesc& desc) override;
	bool UpdateTexture(dy::RHI::ITexture* texture, const void* rgba8Pixels, uint32_t rowPitch) override;
	dy::RHI::IPipelineState* CreateGraphicsPipeline(const dy::RHI::GraphicsPipelineDesc& desc) override;
	dy::RHI::IPipelineState* CreateComputePipeline(const dy::RHI::ComputePipelineDesc& desc) override;
	[[nodiscard]] dy::RHI::DescriptorIndex AllocateDescriptorSlot() override;
	void UpdateDescriptorSlot(dy::RHI::DescriptorIndex index, dy::RHI::ITexture* texture) override;
	void UpdateDescriptorSlot(dy::RHI::DescriptorIndex, dy::RHI::IBuffer*) override {}

	void DestroyBuffer(dy::RHI::IBuffer* buffer) override;
	void DestroyTexture(dy::RHI::ITexture* texture) override;
	void DestroyPipelineState(dy::RHI::IPipelineState* pipeline) override;

	[[nodiscard]] dy::RHI::ITexture* GetBackBuffer() override;
	
	[[nodiscard]] bool RequiresClipSpaceYFlip() const override { return true; }
	[[nodiscard]] bool SupportsSkinningStorageBindings() const override;
	[[nodiscard]] bool SupportsComputeSkinning() const override;
	[[nodiscard]] uint32_t GetValidationErrorCount() const;
	[[nodiscard]] uint32_t GetValidationVuidCount() const;
	[[nodiscard]] bool IsValidationCaptureEnabled() const;
	[[nodiscard]] bool IsDeviceLost() const;
	[[nodiscard]] bool ReadbackTextureRGBA32Float(
		dy::RHI::ITexture* texture,
		std::vector<float>& outPixels);

protected:
	int Initialize(const void* windowHandle, const dy::RHI::DeviceDesc& desc) override;

private:
	std::unique_ptr<Impl> m_impl;
};

}
