#include "Platform/Window.h"
#include "RHI/IDevice.h"
#include "RHI/ICommandList.h"
#include <string.h>
#include <cstdio>

using namespace dy::Platform;

int main()
{
        const char* title = "Renderer";
#if defined(ENABLE_D3D12)
        title = "D3D12 Renderer";
#elif defined(ENABLE_VULKAN)
        title = "Vulkan Renderer";
#endif

        Window window(800, 600, title);

        dy::RHI::IDevice* device = dy::RHI::IDevice::Create(window.GetHandle());
        if (!device)
        {
                printf("Failed to create RHI Device.\n");
                return -1;
        }

        while (window.IsRunning())
        {
                window.PollEvents();

                device->BeginFrame();

                dy::RHI::ICommandList* cmd = device->AcquireCommandList();
                if (!cmd)
                {
                        break;
                }

                cmd->ClearColor(nullptr, 0.0f, 0.0f, 0.0f, 1.0f);
                
                cmd->Close();

                device->Submit(&cmd, 1);

                device->Present();
        }
        delete device;

        return 0;
}
