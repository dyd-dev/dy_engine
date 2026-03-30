#include "App/Window.h"
#include "RHI/RHI.h"
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

	RHIDevice* device = CreateRHIDevice();
	if (!device)
	{
		printf("No RHI backend enabled. Please build with -DB_VULKAN=ON or -DB_D3D12=ON\n");
		window.Release();
		return -1;
	}

	if (!device->Initialize(window.GetHandle(), 800, 600))
	{
		printf("Failed to initialize RHI device\n");
		delete device;
		window.Release();
		return -1;
	}
	
	bool quit = false;
	while (!quit)
	{
		quit = window.PollEvents();

		device->BeginFrame();
		
		RHICommandList* cmd = device->GetCommandList();
		if (!cmd)
		{
			break;
		}

		cmd->Begin();
		cmd->ClearScreen(0.0f, 0.0f, 0.0f, 1.0f);
		// Render
		cmd->End();

		device->SubmitCommandList(cmd);
		
		device->Present(); // to swapchain

		device->EndFrame();
	}
	delete device;
	
	window.Release();
	return 0;
}
