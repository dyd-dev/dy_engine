#include "dyf/Extends/ImGui/ImGui.h"
#include "dyf/Platform/Window.h"
#include "dyf/RHI.h"
#include <imgui.h>
#include <memory>

int main()
{
    dyf::Platform::Window window(800, 600, "dyf / ImGui");
    std::unique_ptr<dyf::RHI::IDevice> device(dyf::RHI::IDevice::Create({}));
    dyf::RHI::SwapchainDesc swapchain;
    swapchain.window = window.GetHandle();
    swapchain.format = dyf::RHI::DefaultSwapchainFormat;
    swapchain.presentMode = dyf::RHI::PresentMode::Fifo;
    (void)device->CreateSwapchain(swapchain);
    auto gui = dyf::Gui::Create(window, *device);

    float color[3] = {0.05f, 0.1f, 0.15f};
    float value = 0.5f;
    bool enabled = true;
    int clicks = 0;

    while(window.IsRunning())
    {
        window.PollEvents();
        if(!window.IsRunning()) break;
        if(!gui->BeginFrame()) continue;

        ImGui::Begin("Widgets");
        ImGui::Text("ImGui widgets rendered through dyf::Gui");
        ImGui::SliderFloat("Value", &value, 0, 1);
        ImGui::Checkbox("Enabled", &enabled);
        ImGui::ColorEdit3("Background", color);
        if(ImGui::Button("Click")) ++clicks;
        ImGui::SameLine();
        ImGui::Text("%d", clicks);
        ImGui::End();
        gui->EndFrame();

        if(!device->BeginFrame()) continue;
        auto* target = device->GetBackBuffer();
        auto* commands = device->AcquireCommandList();
        const dyf::RHI::ResourceBarrierDesc begin{nullptr, target,
            dyf::RHI::ResourceState::Present, dyf::RHI::ResourceState::RenderTarget, {}};
        commands->ResourceBarrier(&begin, 1);
        dyf::RHI::ColorAttachment background;
        background.texture = target;
        background.loadOp = dyf::RHI::LoadOp::Clear;
        background.storeOp = dyf::RHI::StoreOp::Store;
        background.clearColor[0] = color[0];
        background.clearColor[1] = color[1];
        background.clearColor[2] = color[2];
        background.clearColor[3] = 1;
        commands->BeginRendering({&background, 1, nullptr});
        commands->EndRendering();
        const dyf::RHI::ResourceBarrierDesc end{nullptr, target,
            dyf::RHI::ResourceState::RenderTarget, dyf::RHI::ResourceState::Present, {}};
        commands->ResourceBarrier(&end, 1);
        (void)gui->Record(*commands, target);
        (void)commands->Close();
        (void)device->Submit(&commands, 1);
        (void)device->Present();
        device->DestroyCommandList(commands);
    }
    (void)device->WaitIdle();
}
