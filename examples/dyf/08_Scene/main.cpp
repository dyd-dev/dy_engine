#include "dyf.h"

#include <array>
#include <chrono>

int main()
{
	const uint32_t width = 640, height = 480;

	dyf::Platform::Window window(width, height, "dyf / Scene");
	if(!window.GetHandle()) return 1;

	auto renderer = dyf::Renderer::Create(window.GetHandle());
	if(!renderer) return 1;

	dyf::Scene scene;
	const auto mesh = dyf::CreateCubeMesh(1);
	std::array<dyf::EntityHandle, 3> entities;
	const std::array<dyf::Math::float4, 3> colors = {{
		{1, 0.3f, 0.2f, 1}, {0.2f, 0.8f, 1, 1}, {0.5f, 1, 0.3f, 1}
	}};
	for(uint32_t i = 0; i < entities.size(); ++i)
	{
		dyf::MaterialDesc material;
		material.baseColor = colors[i];
		entities[i] = scene.Add(mesh, material);
		if(!entities[i]) return 1;
	}

	dyf::DirectionalLight light;
	light.direction = {0, -1, 2};
	light.color = {1, 1, 1};
	light.intensity = 3;
	if(!scene.Add(light)) return 1;

	dyf::Camera camera;
	dyf::Math::float3 eye = {0, -3, 1.6f}, target = {0, 0, 0};
	if(!camera.LookAt(eye, target)) return 1;
	const float aspect = width / static_cast<float>(height);
	if(!camera.SetPerspective(aspect)) return 1;

	dyf::Math::float3 leftPosition = {-0.8f, 0, 0};
	dyf::Math::float3 lowerPosition = {0.6f, 0, 0.2f}, upperPosition = {0.6f, 0, 0.85f};
	float time = 0;
	auto lastFrame = std::chrono::steady_clock::now();
	while(true)
	{
		window.PollEvents();
		if(!window.IsRunning()) break;

		const auto parent = dyf::Math::RotationZ(time);
		const auto leftTransform = parent * dyf::Math::Translation(leftPosition) * dyf::Math::RotationX(0.4f);
		const auto lowerTransform = parent * dyf::Math::Translation(lowerPosition) * dyf::Math::Scaling(0.6f);
		const auto upperTransform = parent * dyf::Math::Translation(upperPosition) * dyf::Math::Scaling(0.3f);
		if(!entities[0].SetTransform(leftTransform)) return 1;
		if(!entities[1].SetTransform(lowerTransform)) return 1;
		if(!entities[2].SetTransform(upperTransform)) return 1;
		if(!renderer->Render(scene, camera)) return 1;

		const auto now = std::chrono::steady_clock::now();
		const float delta = std::chrono::duration<float>(now - lastFrame).count();
		lastFrame = now;
		time += delta;
	}
}
