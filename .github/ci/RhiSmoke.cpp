#include <iostream>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

#include "Platform/Window.h"
#include "RHI/IBuffer.h"
#include "RHI/ICommandList.h"
#include "RHI/IDevice.h"
#include "RHI/ITexture.h"

int main()
{
	try
	{
		const char* backend =
#if defined(ENABLE_VULKAN)
			"vulkan";
#elif defined(ENABLE_D3D12)
			"d3d12";
#elif defined(ENABLE_METAL)
			"metal";
#else
			"null";
#endif
		if(std::string(backend) == "null") throw std::runtime_error("GPU fixture requires a real graphics backend.");
		std::cout << "[CI fixture] name=rhi-clear backend=" << backend << " case=clear expected=255,0,0" << std::endl;
		dy::Platform::Window window(640, 480, "CI RHI Clear");
		std::unique_ptr<dy::RHI::IDevice> device(dy::RHI::IDevice::Create(window.GetHandle()));
		if(device == nullptr) throw std::runtime_error("Failed to initialize RHI device.");
		const auto before = device->GetResourceAllocationCounters();
		for(uint32_t i = 0; i < 3; ++i)
		{
			dy::RHI::BufferDesc desc{};
			desc.size = 64;
			desc.stride = sizeof(float);
			desc.usage = dy::RHI::BufferUsage::Vertex | dy::RHI::BufferUsage::Storage;
			auto* buffer = device->CreateBuffer(desc);
			if(buffer == nullptr) throw std::runtime_error("Buffer lifecycle: creation failed.");
			void* mapped = buffer->Map(0);
			if(mapped == nullptr)
			{
				device->DestroyBuffer(buffer);
				throw std::runtime_error("Buffer lifecycle: mapping failed.");
			}
			std::memset(mapped, static_cast<int>(i), desc.size);
			buffer->Unmap();
			device->DestroyBuffer(buffer);
		}
		const auto after = device->GetResourceAllocationCounters();
		if(after.buffers.created - before.buffers.created != 3 ||
			after.buffers.destroyed - before.buffers.destroyed != 3 ||
			after.buffers.live != before.buffers.live)
			throw std::runtime_error("Buffer lifecycle: resource counters did not balance.");
		std::cout << "[CI fixture] buffer_lifecycle=ok cycles=3 resource_balance=ok" << std::endl;
		uint64_t frames = 0;

		while(window.IsRunning())
		{
			window.PollEvents();
			if(!window.IsRunning()) break;
			device->BeginFrame();
			dy::RHI::ITexture* backBuffer = device->GetBackBuffer();
			dy::RHI::ICommandList* commandList = device->AcquireCommandList();
			if(backBuffer == nullptr || commandList == nullptr)
				throw std::runtime_error("RHI frame resources are unavailable.");

			commandList->SetRenderTargets(1u, &backBuffer, nullptr);
			commandList->ClearColor(backBuffer, 1.0f, 0.0f, 0.0f, 1.0f);
			commandList->SetViewport({ 0.0f, 0.0f, 640.0f, 480.0f, 0.0f, 1.0f });
			commandList->Close();
			device->Submit(&commandList, 1u);
			device->Present();
			++frames;
		}

		if(frames == 0u) throw std::runtime_error("No frame was submitted.");
		device.reset();
		std::cout << "[CI fixture] shutdown=ok submitted_frames=" << frames << std::endl;
		return 0;
	}
	catch(const std::exception& exception)
	{
		std::cerr << exception.what() << '\n';
		return 1;
	}
}
