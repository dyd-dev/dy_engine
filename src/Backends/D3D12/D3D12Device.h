#pragma once
#include "RHI/RHIDevice.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl.h>

using Microsoft::WRL::ComPtr;

class D3D12Device : public RHIDevice
{
public:
	~D3D12Device();

	bool Initialize(const WindowHandle& windowHandle, uint32_t width, uint32_t height) override;

	void BeginFrame() override;
	void EndFrame() override;

	void SubmitCommandList(RHICommandList* cmd) override;
	void Present() override;

	RHICommandList* GetCommandList() override { return nullptr; }

private:
	void Ms4xMsaaQualityCheck();
	void CreateCommandObjects();
	void CreateSwapChain();

	DXGI_FORMAT m_backBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

	ComPtr<ID3D12Device> m_device;
	ComPtr<IDXGIFactory4> m_dxgiFactory;
	ComPtr<IDXGIAdapter> m_warpAdapter;

	ComPtr<ID3D12Debug> m_debugController;

	ComPtr<ID3D12Fence> m_fence;
	UINT m_rtvDescriptorSize = 0;
	UINT m_dsvDescriptorSize = 0;
	UINT m_cbvSrvUavDescriptorSize = 0;

	bool m_4xMsaaState = false;
	UINT m_4xMsaaQuality = 0;

	ComPtr<ID3D12CommandQueue> m_cmdQueue;
	ComPtr<ID3D12CommandAllocator> m_cmdAllocator;
	ComPtr<ID3D12GraphicsCommandList> m_cmdList;

	ComPtr<IDXGISwapChain> m_swapChain;
};