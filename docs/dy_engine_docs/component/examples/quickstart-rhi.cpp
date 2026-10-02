#include <dyf/Platform/Window.h>
#include <dyf/RHI.h>
#include <chrono>
#include <cstdio>
#include <exception>
#include <memory>
#include <thread>

int main()
{
    try
    {
        constexpr uint32_t width = 640, height = 480;
        dyf::Platform::Window window(width, height, "dy_engine / RHI clear");
        if (!window.GetHandle()) return 1;

        // IDevice::Create는 소유권이 있는 포인터를 반환한다.
        std::unique_ptr<dyf::RHI::IDevice> device(
            dyf::RHI::IDevice::Create(dyf::RHI::DeviceDesc{}));
        if (!device) return 1;

        dyf::RHI::SwapchainDesc swapchain;
        swapchain.window = window.GetHandle();
        swapchain.format = dyf::RHI::Format::B8G8R8A8_UNORM;
        // 네이티브 창은 0으로 크기를 유도하고, Null 검증은 명시 크기를 요구한다.
        if (!device->Supports(dyf::RHI::Feature::Rasterization))
        {
            swapchain.initialWidth = width;
            swapchain.initialHeight = height;
        }
        if (!device->CreateSwapchain(swapchain)) return 1;

        while (window.IsRunning())
        {
            window.PollEvents();
            if (!window.IsRunning()) break;
            if (!device->BeginFrame())
            {
                if (device->IsLost()) return 1;
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                continue;
            }

            // ResourceScope는 device보다 먼저 파괴된다.
            dyf::RHI::ResourceScope frameResources(*device);
            auto* commands = frameResources.Keep(device->AcquireCommandList());
            auto* backBuffer = device->GetBackBuffer();
            if (!backBuffer) return 1; // 스왑체인 소유이므로 Keep/DestroyTexture 금지.

            const dyf::RHI::ResourceBarrierDesc begin{
                nullptr, backBuffer, dyf::RHI::ResourceState::Present,
                dyf::RHI::ResourceState::RenderTarget, {}};
            commands->ResourceBarrier(&begin, 1);

            dyf::RHI::ColorAttachment color;
            color.texture = backBuffer;
            color.loadOp = dyf::RHI::LoadOp::Clear;
            color.storeOp = dyf::RHI::StoreOp::Store;
            color.clearColor[0] = 0.04f;
            color.clearColor[1] = 0.06f;
            color.clearColor[2] = 0.10f;
            color.clearColor[3] = 1.0f;
            commands->BeginRendering({&color, 1, nullptr});
            commands->EndRendering();

            const dyf::RHI::ResourceBarrierDesc end{
                nullptr, backBuffer, dyf::RHI::ResourceState::RenderTarget,
                dyf::RHI::ResourceState::Present, {}};
            commands->ResourceBarrier(&end, 1);

            // 기록 실패는 Close, 제출 실패는 Submit에서 확인한다.
            if (!commands->Close()) return 1;
            if (!device->Submit(&commands, 1)) return 1;
            if (!device->Present()) return 1;
        }
        return device->WaitIdle() ? 0 : 1;
    }
    catch (const std::exception& error)
    {
        // ResourceScope::Keep(nullptr)도 예외로 보고된다.
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}