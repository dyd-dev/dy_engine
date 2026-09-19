#include "dyf.h"

#define DY_EXAMPLE_IMAGE "Assets/Default_albedo.jpg"

int main()
{
	const uint32_t width = 640, height = 480;

	dyf::Platform::Window window(width, height, "dyf / Image");
	if(!window.GetHandle()) return 1;
	
	auto renderer = dyf::Renderer::Create(window.GetHandle());
	if(!renderer) return 1;

	dyf::Canvas canvas(width,height);
	
	dyf::Math::float4 color = {0.04f,0.06f,0.10f,1.0f};
	canvas.Clear(color);
	
	dyf::Image image;
	if(!dyf::LoadImage(DY_EXAMPLE_IMAGE, image)) return 1;

	dyf::Rectangle dest;
	canvas.Image(image, dest = {40,70,240,240});
	canvas.Image(image, dest = {340,110,240,240}, color = {1,0.5f,0.4f,0.65f});

	while(true)
	{
		window.PollEvents();
		if(!window.IsRunning()) break;
		
		if(!renderer->Render(canvas)) return 1;
	}
}
