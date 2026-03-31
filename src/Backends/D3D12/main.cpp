#include "Platform/Window.h"
#include "RHI/IDevice.h"
#include <iostream>

using namespace dy;

int main()
{
	Platform::Window window(800, 600, "DyEngine - D3D12 Test");
	RHI::IDevice* device = RHI::IDevice::Create(window.GetHandle());
	
	if (!device) {
		std::cerr << "D3D12 Device 생성 실패! << std::endl";
		return -1;
	}
	while (window.IsRunning())
	{
		window.PollEvents();

		device->BeginFrame();
		device->EndFrame();
		device->Present();
	}
	
	delete device;
	return 0;
}