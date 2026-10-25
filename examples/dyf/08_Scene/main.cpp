#include "dyf.h"

#include <array>
#include <chrono>

int main()
{
	const uint32_t width = 640, height = 480;

	dyf::Platform::Window window(width, height, "dyf / Scene");

	auto renderer = dyf::Renderer::Create(window.GetHandle());

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
	}

	dyf::DirectionalLight light;
	light.direction = {0, -1, 2};
	light.color = {1, 1, 1};
	light.intensity = 3;
	scene.Add(light);

	dyf::Camera camera;
	dyf::Math::float3 eye = {0, -3, 1.6f}, target = {0, 0, 0};
	camera.LookAt(eye, target);
	const float aspect = width / static_cast<float>(height);
	camera.SetPerspective(aspect);

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
		entities[0].SetTransform(leftTransform);
		entities[1].SetTransform(lowerTransform);
		entities[2].SetTransform(upperTransform);
		renderer->Render(scene, camera);

		const auto now = std::chrono::steady_clock::now();
		const float delta = std::chrono::duration<float>(now - lastFrame).count();
		lastFrame = now;
		time += delta;
	}
	return 0;
}
