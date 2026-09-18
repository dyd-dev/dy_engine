#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "dyf/Platform/Window.h"
#include "dyf/RHI.h"

#ifndef DY_CI_BACKEND
#error DY_CI_BACKEND must name the configured graphics backend.
#endif

namespace
{
void Require(bool condition, const char* message)
{
	if(!condition) throw std::runtime_error(message);
}
}

int main()
{
	try
	{
		using namespace dyf;
		Require(std::string(DY_CI_BACKEND) != "null", "GPU fixture requires a real graphics backend.");
		std::cout << "[CI fixture] name=rhi-clear backend=" << DY_CI_BACKEND
				  << " case=clear expected=255,0,0" << std::endl;
		Platform::Window window(640, 480, "CI RHI Clear");
		Require(window.GetHandle() != nullptr, "Failed to create fixture window.");
		std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create({}));
		Require(device != nullptr, "Failed to initialize RHI device.");
		RHI::SwapchainDesc swapchain;
		swapchain.window = window.GetHandle();
		swapchain.format = RHI::Format::B8G8R8A8_UNORM;
		Require(device->CreateSwapchain(swapchain), "Failed to create fixture swapchain.");

		for(uint32_t i = 0; i < 3; ++i)
		{
			RHI::ResourceScope resources(*device);
			auto* buffer = resources.Keep(device->CreateBuffer({64, sizeof(float),
				RHI::BufferUsage::Vertex | RHI::BufferUsage::Storage, RHI::ResourceState::CopyDestination}));
			auto* upload = resources.Keep(device->AcquireCommandList());
			std::array<uint8_t, 64> bytes;
			bytes.fill(static_cast<uint8_t>(i));
			Require(device->UpdateBuffer(*upload, buffer, 0, bytes.data(), static_cast<uint32_t>(bytes.size())),
					"Buffer lifecycle: upload failed.");
			const RHI::ResourceBarrierDesc ready{buffer, nullptr,
				RHI::ResourceState::CopyDestination, RHI::ResourceState::VertexBuffer, {}};
			upload->ResourceBarrier(&ready, 1);
			Require(upload->Close(), "Buffer lifecycle: recording failed.");
			Require(device->Submit(&upload, 1), "Buffer lifecycle: submission failed.");
			Require(device->WaitIdle(), "Buffer lifecycle: completion failed.");
		}
		// The public API exposes completion and destruction, but no allocation counters.
		std::cout << "[CI fixture] buffer_lifecycle=ok cycles=3 resource_lifecycle=ok" << std::endl;
		uint64_t frames = 0;
		while(window.IsRunning())
		{
			window.PollEvents();
			if(!window.IsRunning()) break;
			if(!device->BeginFrame())
			{
				Require(!device->IsLost(), "RHI device lost while acquiring a frame.");
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				continue;
			}
			RHI::ResourceScope resources(*device);
			auto* target = device->GetBackBuffer();
			auto* commands = resources.Keep(device->AcquireCommandList());
			Require(target != nullptr, "RHI backbuffer is unavailable.");
			const RHI::ResourceBarrierDesc begin{nullptr, target,
				RHI::ResourceState::Present, RHI::ResourceState::RenderTarget, {}};
			commands->ResourceBarrier(&begin, 1);
			RHI::ColorAttachment color;
			color.texture = target;
			color.loadOp = RHI::LoadOp::Clear;
			color.storeOp = RHI::StoreOp::Store;
			color.clearColor[0] = color.clearColor[3] = 1;
			commands->BeginRendering({&color, 1, nullptr});
			commands->EndRendering();
			const RHI::ResourceBarrierDesc present{nullptr, target,
				RHI::ResourceState::RenderTarget, RHI::ResourceState::Present, {}};
			commands->ResourceBarrier(&present, 1);
			Require(commands->Close(), "RHI frame recording failed.");
			Require(device->Submit(&commands, 1), "RHI frame submission failed.");
			Require(device->Present(), "RHI frame presentation failed.");
			++frames;
		}

		Require(frames != 0, "No frame was submitted.");
		Require(device->WaitIdle() && !device->IsLost(), "RHI shutdown completion failed.");
		device.reset();
		std::cout << "[CI fixture] shutdown=ok submitted_frames=" << frames << std::endl;
		return 0;
	}
	catch(const std::exception& exception)
	{
		std::cerr << "[CI fixture] failure=" << exception.what() << '\n';
		return 1;
	}
}
