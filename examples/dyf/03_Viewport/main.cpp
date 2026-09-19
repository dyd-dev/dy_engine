#include "dyf.h"

int main()
{
	const uint32_t width = 640, height = 480;

	dyf::Platform::Window window(width, height, "dyf / Viewport and clip");
	if(!window.GetHandle()) return 1;

	auto renderer = dyf::Renderer::Create(window.GetHandle());
	if(!renderer) return 1;

	dyf::Canvas canvas(width, height);
	dyf::Math::float4 color = {0.04f, 0.06f, 0.10f, 1};
	canvas.Clear(color);

	dyf::Rectangle viewport = {0, 0, 320, 480};
	if(!canvas.SetViewport(viewport)) return 1;

	dyf::Rectangle panel = {20, 20, 280, 440};
	color = {0.1f, 0.2f, 0.4f, 1};
	canvas.FillRect(panel, color);

	dyf::Rectangle clip = {60, 100, 200, 240};
	canvas.SetClipRect(clip);

	dyf::Math::float2 center = {160, 240};
	const float radius = 150;
	color = {1, 0.4f, 0.2f, 1};
	canvas.FillCircle(center, radius, color);

	viewport = {320, 0, 320, 480};
	if(!canvas.SetViewport(viewport)) return 1;

	color = {0.15f, 0.35f, 0.2f, 1};
	canvas.FillRect(panel, color);

	dyf::Math::float2 start = {0, 0}, end = {320, 480};
	color = {1, 1, 1, 1};
	canvas.Line(start, end, color, 12);

	while(true)
	{
		window.PollEvents();
		if(!window.IsRunning()) break;

		if(!renderer->Render(canvas)) return 1;
	}
}
