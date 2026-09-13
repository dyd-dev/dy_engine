#include "dyf.h"

int main()
{
	const uint32_t width = 640, height = 480;

	dyf::Platform::Window window(width, height, "dyf / Primitives");
	if(!window.GetHandle()) return 1;
	
	auto renderer = dyf::Renderer::Create(window.GetHandle());
	if(!renderer) return 1;

	dyf::Math::float4 color = {};

	dyf::Canvas canvas(width,height);
	canvas.Clear(color={0.04f,0.06f,0.10f,1.0f});

	dyf::Math::float2 position = {70,70};
	canvas.Point(position, color={1,0.7f,0.1f,1}, 10);

	dyf::Math::float2 start = {100,70}, end = {260,130};
	canvas.Line(start, end, color={0.1f,0.8f,1,1}, 6);

	dyf::Rectangle rect = {};
	canvas.Rect(rect={320,45,200,100}, color={1,0.3f,0.4f,1}, 5);
	canvas.FillRect(rect={45,210,180,120}, color={0.2f,0.8f,0.4f,1});

	dyf::Math::float2 center = {500,150};
	float radius = 60.0f;
	canvas.Circle(center, radius, color={0.8f,0.5f,1,1}, 4);
	canvas.FillCircle(center={510,300}, radius=70, color={1,0.65f,0.15f,1});

	while (true)
	{
		window.PollEvents();
		if(!window.IsRunning()) break;
		
		if(!renderer->Render(canvas)) return 1;
	}
}
