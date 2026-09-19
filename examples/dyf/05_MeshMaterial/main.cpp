#include "dyf.h"

#define DY_EXAMPLE_IMAGE "Assets/Default_albedo.jpg"

int main()
{
	const uint32_t width = 640, height = 480;

	dyf::Platform::Window window(width, height, "dyf / Mesh and Material");
	if(!window.GetHandle()) return 1;

	auto renderer = dyf::Renderer::Create(window.GetHandle());
	if(!renderer) return 1;

	dyf::Scene scene;
	const auto mesh = dyf::CreateCubeMesh(1);

	dyf::MaterialDesc textured;
	if(!dyf::LoadImage(DY_EXAMPLE_IMAGE, textured.baseColorTexture)) return 1;
	textured.roughnessFactor = 0.8f;

	dyf::MaterialDesc metallic;
	metallic.baseColor = {0.9f, 0.45f, 0.15f, 1};
	metallic.metallicFactor = 0.85f;
	metallic.roughnessFactor = 0.2f;

	dyf::Math::float3 leftPosition = {-0.7f, 0, 0}, rightPosition = {0.7f, 0, 0};
	const auto leftTransform = dyf::Math::Translation(leftPosition) * dyf::Math::RotationZ(0.35f);
	const auto rightTransform = dyf::Math::Translation(rightPosition) * dyf::Math::RotationZ(-0.35f);
	const auto left = scene.Add(mesh, textured, leftTransform);
	if(!left) return 1;
	const auto right = scene.Add(mesh, metallic, rightTransform);
	if(!right) return 1;

	dyf::DirectionalLight light;
	light.direction = {0, -1, 2};
	light.color = {1, 1, 1};
	light.intensity = 3;
	const auto lightId = scene.Add(light);
	if(!lightId) return 1;

	dyf::Camera camera;
	dyf::Math::float3 eye = {0, -3, 1.6f}, target = {0, 0, 0};
	if(!camera.LookAt(eye, target)) return 1;
	const float aspect = width / static_cast<float>(height);
	if(!camera.SetPerspective(aspect)) return 1;

	while(true)
	{
		window.PollEvents();
		if(!window.IsRunning()) break;

		if(!renderer->Render(scene, camera)) return 1;
	}
}
