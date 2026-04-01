#include <cstdint> // for uint32_t
#include "Platform/Window.h"
#include "RHI/IDevice.h"
#include "Graphics/Renderer.h"
#include "Graphics/Scene.h"
#include "Graphics/Camera.h"

using namespace dy;

int main()
{
	Platform::Window window(800,600,"Hello,Renderer");
	RHI::IDevice *device = RHI::IDevice::Create(window.GetHandle());

	Graphics::Renderer renderer(device);
	Graphics::Camera camera;

	camera.SetLookAt(
		Math::Vector3(0.0f, 5.0f, -10.0f),
		Math::Vector3(0.0f, 0.0f, 0.0f),
		Math::Vector3(0.0f, 1.0f, 0.0f)
	);
	camera.SetPerspective(
		45.0f, 1920.0f / 1080.0f, 0.1f, 1000.0f
	);

	while(window.IsRunning())
	{
		window.PollEvents();

		// [PHASE A] Game Logic & Physics (Pure CPU / Memory Bound)

		camera.UpdateMatrices();

		// [ PHASE B ] RHI Frame Synchronization
		// -----------------------------------------------------------------
        // Waits for swapchain, acquires the current frame index.
		device->BeginFrame();

		// [ PHASE C ] Render Submission (Data -> Execution Bridge)
		// -----------------------------------------------------------------
        // The Renderer reads the Scene's arrays, culls invisible objects, 
        // generates DrawPackets, sorts them, and records to the CommandList.
		renderer.RenderFrame();

		// [ PHASE D ] Present to Display
		device->Present();
	}
	delete device;
	return 0;
}