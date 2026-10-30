#include "dyf.h"

// 사용할 폰트 경로는 이 파일에서 지정한다.
#if defined(_WIN32)
#define DY_EXAMPLE_FONT "C:/Windows/Fonts/malgun.ttf"
#elif defined(__APPLE__)
#define DY_EXAMPLE_FONT "/System/Library/Fonts/Helvetica.ttc"
#elif defined(__ANDROID__)
#define DY_EXAMPLE_FONT "/system/fonts/Roboto-Regular.ttf"
#else
#define DY_EXAMPLE_FONT "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
#endif

int main()
{
	const uint32_t width = 640, height = 480;

	dyf::Platform::Window window(width, height, "dyf / Text");

	auto renderer = dyf::Renderer::Create(window.GetHandle());

	auto font = dyf::Font::Load(DY_EXAMPLE_FONT, 32);

	dyf::Canvas canvas(width, height);
	dyf::Math::float4 color = {0.04f, 0.06f, 0.10f, 1};
	canvas.Clear(color);

	dyf::Math::float2 position = {30, 70};
	const char* description = "Renderer = RHI commands\nText, kerning and alpha blending";
	canvas.Text(*font, description, position);

	position = {30, 190};
	color = {0.3f, 0.8f, 1, 1};
	canvas.Text(*font, "0123456789  AVATAR", position, color);

	dyf::Rectangle clip = {60, 260, 480, 60};
	canvas.SetClipRect(clip);

	position = {30, 270};
	color = {1, 0.7f, 0.2f, 0.75f};
	canvas.Text(*font, "Clipped text across the viewport", position, color);

	while(true)
	{
		window.PollEvents();
		if(!window.IsRunning()) break;

		renderer->Render(canvas);
	}
	return 0;
}
