#include "dyf.h"
#include "dyf/Platform/Time.h"
#include <algorithm>
#include <cmath>

int main()
{
	const uint32_t width = 640, height = 480;

	dyf::Platform::Window window(width, height, "Geometry and Camera - WASD | RMB look | Wheel speed | Esc");
	if(!window.GetHandle()) return 1;

	auto renderer = dyf::Renderer::Create(window.GetHandle());
	if(!renderer) return 1;

	const dyf::Math::float3 positions[] = {
		{-0.9f, 0, -0.6f}, {0.9f, 0, -0.6f}, {0, 0, 0.9f}
	};
	dyf::MeshData triangle;
	for(const auto& position : positions)
	{
		dyf::Vertex vertex;
		vertex.position = position;
		vertex.normal = {0, -1, 0};
		vertex.uv = {0, 0};
		triangle.vertices.push_back(vertex);
	}
	triangle.indices = {0, 1, 2};

	dyf::MaterialDesc material;
	material.baseColor = {0.2f, 0.7f, 1, 1};

	dyf::Camera camera;
	dyf::Math::float3 eye = {0, -3, 1.6f}, target = {0, 0, 0};
	if(!camera.LookAt(eye, target)) return 1;
	const float aspect = width / static_cast<float>(height);
	if(!camera.SetPerspective(aspect)) return 1;

	const auto initialForward = dyf::Math::Normalize(target - eye);
    float yaw = std::atan2(initialForward.y, initialForward.x);
    float pitch = std::asin(initialForward.z), moveSpeed = 2.5f;
    dyf::Platform::Time time;
	while(true)
	{
		window.PollEvents();
        const auto& input = window.GetInput();
        using dyf::Platform::Key;
        using dyf::Platform::MouseButton;
        if(input.WasPressed(Key::Escape)) window.RequestClose();
		if(!window.IsRunning()) break;
        const auto size = window.GetFramebufferSize();
        const bool active = window.HasFocus() && !window.IsMinimized() && size.width > 0 && size.height > 0;
        const float delta = static_cast<float>((std::min)(time.Tick(!active), 0.1));
        window.SetCursorMode(active && input.IsDown(MouseButton::Right)
            ? dyf::Platform::CursorMode::Locked : dyf::Platform::CursorMode::Normal);
        if(!active) { dyf::Platform::Window::WaitEvents(); continue; }
        if(input.IsDown(MouseButton::Right))
        {
            const auto mouse = input.GetCursorDelta();
            yaw -= static_cast<float>(mouse.x) * 0.003f;
            pitch = std::clamp(pitch - static_cast<float>(mouse.y) * 0.003f, -1.5f, 1.5f);
        }
        const dyf::Math::float3 forward(std::cos(pitch)*std::cos(yaw), std::cos(pitch)*std::sin(yaw), std::sin(pitch));
        const auto right = dyf::Math::Normalize(dyf::Math::Cross(forward, {0,0,1}));
        const float forwardAxis = (input.IsDown(Key::W) ? 1.f : 0.f) - (input.IsDown(Key::S) ? 1.f : 0.f);
        const float rightAxis = (input.IsDown(Key::D) ? 1.f : 0.f) - (input.IsDown(Key::A) ? 1.f : 0.f);
        moveSpeed = std::clamp(moveSpeed + static_cast<float>(input.GetScrollDelta().y)*0.5f, 0.25f, 20.f);
        const auto movement = forward * forwardAxis + right * rightAxis;
        if(dyf::Math::LengthSquared(movement) > 0) eye = eye + dyf::Math::Normalize(movement) * (moveSpeed * delta);
        if(!camera.LookAt(eye, eye + forward) || !camera.SetPerspective(float(size.width)/size.height)) return 1;

		if(!renderer->Render(triangle, material, camera)) return 1;
	}
}
