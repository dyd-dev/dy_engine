#include "D3D12Device.h"
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <Windows.h>
#include <exception>
#include <cstdio>

#define SDL_Log(fmt, ...) { printf(fmt, ##__VA_ARGS__); printf("\n"); }

/* DirectXMath.h

	XMVECTOR 등을 활용해야 SIMD의 장점을 취할 수 있다. SSE2

Windows.h
	XMVerifyCPUSupport()
*/

static void ThrowIfFailed(HRESULT hr)
{
	if (FAILED(hr)) // 중단점 설정
		throw std::exception();
}

D3D12Device::~D3D12Device()
{
	if (m_swapChain) m_swapChain.Reset();

	if (m_cmdList) m_cmdList.Reset();
	if (m_cmdAllocator) m_cmdAllocator.Reset();
	if (m_cmdQueue) m_cmdQueue.Reset();

	if (m_fence) m_fence.Reset();

	if (m_debugController) m_debugController.Reset();

	if (m_warpAdapter) m_warpAdapter.Reset();
	if (m_dxgiFactory) m_dxgiFactory.Reset();
	if (m_device) m_device.Reset();
}

bool D3D12Device::Initialize(const WindowHandle& windowHandle, uint32_t width, uint32_t height)
{
	GLFWwindow* window = static_cast<GLFWwindow*>(windowHandle.windowPointer);
	HWND hwnd = glfwGetWin32Window(window);

#if defined(DEBUG) || defined(_DEBUG)
	{
		ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&m_debugController)));
		m_debugController->EnableDebugLayer();
	}
#endif
	ThrowIfFailed(CreateDXGIFactory(IID_PPV_ARGS(&m_dxgiFactory)));

	HRESULT hardwareResult = D3D12CreateDevice(
		nullptr,
		D3D_FEATURE_LEVEL_11_0,
		IID_PPV_ARGS(&m_device)
	);
	if (FAILED(hardwareResult))
	{
		ThrowIfFailed(m_dxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&m_warpAdapter)));

		ThrowIfFailed(D3D12CreateDevice(
			m_warpAdapter.Get(),
			D3D_FEATURE_LEVEL_11_0,
			IID_PPV_ARGS(&m_device)
		));
	}

	ThrowIfFailed(m_device->CreateFence(
		0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));

	m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	m_dsvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	m_cbvSrvUavDescriptorSize = m_device->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	return true;
}

void D3D12Device::BeginFrame()
{
}

void D3D12Device::EndFrame()
{
}

void D3D12Device::SubmitCommandList(RHICommandList* cmd)
{
}

void D3D12Device::Present()
{
}

void D3D12Device::Ms4xMsaaQualityCheck()
{
	D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS msQualityLevels;
	msQualityLevels.Format = m_backBufferFormat;
	msQualityLevels.SampleCount = 4;
	msQualityLevels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
	msQualityLevels.NumQualityLevels = 0;
	ThrowIfFailed(m_device->CheckFeatureSupport(
		D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
		&msQualityLevels,
		sizeof(msQualityLevels)
	));

	m_4xMsaaQuality = msQualityLevels.NumQualityLevels;
	if (m_4xMsaaQuality > 0) m_4xMsaaState = true;
	else
		SDL_Log("4X MSAA is not supported.");
}

void D3D12Device::CreateCommandObjects()
{
	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

	ThrowIfFailed(m_device->CreateCommandQueue(
		&queueDesc, IID_PPV_ARGS(&m_cmdQueue)
	));

	ThrowIfFailed(m_device->CreateCommandAllocator(
		queueDesc.Type, IID_PPV_ARGS(&m_cmdAllocator)
	));

	ThrowIfFailed(m_device->CreateCommandList(
		0,
		queueDesc.Type,
		m_cmdAllocator.Get(),
		nullptr,
		IID_PPV_ARGS(&m_cmdList)
	));

	// Start with the command list in the closed state. Because the first time we refer to the command list, we will Reset it, and it needs to be closed before calling Reset.
	m_cmdList->Close();
}

void D3D12Device::CreateSwapChain()
{
	m_swapChain.Reset();

	DXGI_SWAP_CHAIN_DESC sd = {};
	sd.BufferDesc.Width = 0; // Use automatic sizing.
	sd.BufferDesc.Height = 0;
	sd.BufferDesc.RefreshRate.Numerator = 60;
	sd.BufferDesc.RefreshRate.Denominator = 1;
	sd.BufferDesc.Format = m_backBufferFormat;
	sd.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
	sd.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
	sd.SampleDesc.Count = m_4xMsaaState ? 4 : 1;
	sd.SampleDesc.Quality = m_4xMsaaState ? (m_4xMsaaQuality - 1) : 0;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;



	ThrowIfFailed(m_dxgiFactory->CreateSwapChain(
		m_cmdQueue.Get(),
		&sd,
		m_swapChain.GetAddressOf()
	));
}