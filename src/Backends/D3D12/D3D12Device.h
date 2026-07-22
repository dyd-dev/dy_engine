#pragma once
#include "RHI/IDevice.h"

namespace dy::Backends {
// Pimpl (Pointer to Implementation) 패턴
// DX12의 실제 객체들(ComPtr 등)을 이 구조체 안에 숨길 예정입니다.
struct D3D12InternalState;

class D3D12Device : public RHI::IDevice {
public:
  D3D12Device();
  ~D3D12Device() override;

  // --- IDevice Interface Overrides ---
  bool BeginFrame() override;
  uint32_t GetCurrentFrameIndex() const override;

  RHI::ICommandList *AcquireCommandList() override;
  void Submit(RHI::ICommandList **cmdLists, uint32_t count) override;
  void Present() override;

  RHI::IBuffer *CreateBuffer(const RHI::BufferDesc &desc) override;
  RHI::IShader *CreateShader(const RHI::ShaderDesc &desc) override;
  RHI::ISampler *CreateSampler(const RHI::SamplerDesc &desc) override;
  RHI::ITexture *CreateTexture(const RHI::TextureDesc &desc) override;
  bool UpdateTexture(RHI::ITexture *texture, const void *data, uint32_t rowPitch, RHI::ResourceState finalState) override;
  RHI::IPipelineState *CreateGraphicsPipeline(const RHI::GraphicsPipelineDesc &desc) override;
  RHI::IResourceSet *CreateResourceSet(RHI::IPipelineState *pipeline) override;
  bool UpdateResourceSet(RHI::IResourceSet *resourceSet, const RHI::ResourceSetWrite *writes, uint32_t writeCount) override;

  void DestroyBuffer(RHI::IBuffer *buffer) override;
  void DestroyShader(RHI::IShader *shader) override;
  void DestroySampler(RHI::ISampler *sampler) override;
  void DestroyTexture(RHI::ITexture *texture) override;
  void DestroyPipelineState(RHI::IPipelineState *pipeline) override;
  void DestroyResourceSet(RHI::IResourceSet *resourceSet) override;

  RHI::ITexture *GetBackBuffer() override;

protected:
  int Initialize(const void *windowHandle, const RHI::DeviceDesc& desc) override;

private:
  D3D12InternalState *m_internal; // DX12 관련 모든 변수는 이 안에 들어갑니다.
};
} // namespace dy::Backends
