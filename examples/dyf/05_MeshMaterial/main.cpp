#include "dyf.h"

#define DY_EXAMPLE_IMAGE "Assets/Default_albedo.jpg"

int main()
{
	const uint32_t width = 640, height = 480;

	dyf::Platform::Window window(width, height, "dyf / Mesh and Material");

	auto renderer = dyf::Renderer::Create(window.GetHandle());

	dyf::Scene scene;
	const auto mesh = dyf::CreateCubeMesh(1);

	dyf::MaterialDesc textured;
	dyf::LoadImage(DY_EXAMPLE_IMAGE, textured.baseColorTexture);
	textured.roughnessFactor = 0.8f;

	dyf::MaterialDesc metallic;
	metallic.baseColor = {0.9f, 0.45f, 0.15f, 1};
	metallic.metallicFactor = 0.85f;
	metallic.roughnessFactor = 0.2f;

	dyf::Math::float3 leftPosition = {-0.7f, 0, 0}, rightPosition = {0.7f, 0, 0};
	const auto leftTransform = dyf::Math::Translation(leftPosition) * dyf::Math::RotationZ(0.35f);
	const auto rightTransform = dyf::Math::Translation(rightPosition) * dyf::Math::RotationZ(-0.35f);
	scene.Add(mesh, textured, leftTransform);
	scene.Add(mesh, metallic, rightTransform);

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

	while(true)
	{
		window.PollEvents();
		if(!window.IsRunning()) break;

		renderer->Render(scene, camera);
	}
	return 0;
}
