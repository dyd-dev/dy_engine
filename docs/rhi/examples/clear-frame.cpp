#include <Platform/Window.h>
#include <RHI/ICommandList.h>
#include <RHI/IDevice.h>
#include <iostream>
#include <memory>
#include <stdexcept>

int main()
{
    using namespace dy::RHI;
    try
    {
        dy::Platform::Window window(960, 540, "RHI / Clear frame");
        std::unique_ptr<IDevice> device(IDevice::Create(window.GetHandle()));
        if (!device) throw std::runtime_error("Device creation failed.");

        SwapchainDesc swapchain;
        swapchain.format = Format::B8G8R8A8_UNORM;
        swapchain.minimumImageCount = 2;
        swapchain.presentMode = PresentMode::Fifo;
        if (!device->CreateSwapchain(swapchain))
            throw std::runtime_error("Swapchain creation failed.");

        while (window.IsRunning())
        {
            window.PollEvents();
            if (!window.IsRunning()) break;
            if (!device->BeginFrame()) continue;

            ICommandList* commands = device->AcquireCommandList();
            if (!commands) throw std::runtime_error("No command list.");
            TextureHandle backBuffer = device->GetBackBuffer();

            const ResourceBarrierDesc toRenderTarget{
                nullptr, backBuffer,
                ResourceState::Present, ResourceState::RenderTarget, {}};
            commands->ResourceBarrier(&toRenderTarget, 1);

            ColorAttachment color;
            color.texture = backBuffer;
            color.loadOp = LoadOp::Clear;
            color.storeOp = StoreOp::Store;
            color.clearColor[0] = 0.04f;
            color.clearColor[1] = 0.08f;
            color.clearColor[2] = 0.16f;
            color.clearColor[3] = 1.0f;
            commands->BeginRendering({&color, 1, nullptr});
            commands->EndRendering();

            const ResourceBarrierDesc toPresent{
                nullptr, backBuffer,
                ResourceState::RenderTarget, ResourceState::Present, {}};
            commands->ResourceBarrier(&toPresent, 1);
            commands->Close();

            // Owned, closed lists are consumed even when Submit fails.
            if (!device->Submit(&commands, 1))
                throw std::runtime_error("Frame submission failed.");
            device->Present();
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
