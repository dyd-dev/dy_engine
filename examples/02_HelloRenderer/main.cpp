#include <cstdint> // for uint32_t
#include "Platform/Window.h"
#include "Graphics/JobSystem.h"
#include "RHI/IDevice.h"
#include "Graphics/Scene.h"
#include "Graphics/Renderer.h"

using namespace dy;

int main()
{
	Platform::Window window(800,600,"Hello,Renderer");
	
	RHI::IDevice *device = RHI::IDevice::Create(window.GetHandle());

	JobSystem jobSystem;
	jobSystem.Initialize();

	Graphics::Renderer renderer;
	renderer.Initialize(device);
	
	Scene scene;
	const uint32_t ENTITY_COUNT = 50000;
	for(uint32_t i=0; i<ENTITY_COUNT; i++)
	{
		scene.CreateEntity();
	}

	float deltaTime = 0.016f;

	while(window.IsRunning())
	{
		window.PollEvents();

		device->BeginFrame();

		jobSystem.ParallelFor(scene.GetActiveCount(), [&scene, deltaTime](uint32_t startIdx, uint32_t endIdx)
		{
			for(uint32_t i = startIdx; i < endIdx; i++)
			{
				EntityID id = static_cast<EntityID>(i);
				TransformData& t = scene.GetTransform(id);
			}
		});
		jobSystem.WaitForAll();

		renderer.SubmitFrame(scene, device, &jobSystem);
		
		device->Present();
	}
	jobSystem.Shutdown();
	renderer.Shutdown(device);

	delete device;
	return 0;
}