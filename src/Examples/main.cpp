#include "App/Window.h"
#include "RHI/IDevice.h"
#include "RHI/ICommandList.h"
#include <string.h>
#include <cstdio>

int main()
{
        const char* title = "Renderer";
#if defined(ENABLE_D3D12)
        title = "D3D12 Renderer";
#elif defined(ENABLE_VULKAN)
        title = "Vulkan Renderer";
#endif

        Window window;
        if(!window.Initialize(title, 800, 600))
        {
                printf("Failed to initialize window\n");
                return -1;
        }

        // Initialize is called internally inside Create
        dy::RHI::IDevice* device = dy::RHI::IDevice::Create(window.GetHandle().windowPointer);
        if (!device)
        {
                printf("No RHI backend enabled or failed to initialize. Please build with -DB_VULKAN=ON or -DB_D3D12=ON\n");
                window.Release();
                return -1;
        }

        bool quit = false;
        while (!quit)
        {
                quit = window.PollEvents();

                device->BeginFrame();

                dy::RHI::ICommandList* cmd = device->AcquireCommandList();
                if (!cmd)
                {
                        break;
                }

                // cmd->Begin(); // Begin is implicitly handled by AcquireCommandList in typical designs, or implemented internally
                cmd->ClearColor(nullptr, 0.0f, 0.0f, 0.0f, 1.0f);
                // Render
                cmd->Close();

                device->Submit(&cmd, 1);

                device->Present(); // to swapchain
        }
        delete device;

        window.Release();
        return 0;
}
