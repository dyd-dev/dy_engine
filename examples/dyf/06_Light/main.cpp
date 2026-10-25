#include "dyf.h"
#include "dyf/Extends/AdvancedRendering.h"

int main()
{
	const uint32_t width = 640, height = 480;

	dyf::Platform::Window window(width, height, "dyf / Five light types and shadows");

	auto renderer = dyf::Renderer::Create(window.GetHandle());
	renderer->AddExtension(dyf::CreateAdvancedRenderingModule(dyf::AdvancedRenderModule::ClusteredLighting));

	renderer->SetShadowsEnabled(true);

	dyf::Scene scene;
	const auto mesh = dyf::CreateCubeMesh(1);
	dyf::MaterialDesc material;
	material.roughnessFactor = 0.65f;

	dyf::Math::float3 leftPosition = {-0.7f, 0, 0}, rightPosition = {0.7f, 0, 0};
	dyf::Math::float3 groundPosition = {0, 0, -0.65f}, groundScale = {3, 3, 0.1f};
	const auto leftTransform = dyf::Math::Translation(leftPosition);
	const auto rightTransform = dyf::Math::Translation(rightPosition);
	const auto groundTransform = dyf::Math::Translation(groundPosition) * dyf::Math::Scaling(groundScale);
	scene.Add(mesh, material, leftTransform);
	scene.Add(mesh, material, rightTransform);
	scene.Add(mesh, material, groundTransform);

	dyf::DirectionalLight directional;
	directional.direction = {0, -1, 2};
	directional.color = {1, 1, 1};
	directional.intensity = 0.3f;
	scene.Add(directional);

	dyf::PointLight point;
	point.position = {-1, -1, 1.4f};
	point.range = 5;
	point.color = {1, 0.2f, 0.1f};
	point.intensity = 3;
	scene.Add(point);

	dyf::SpotLight spot;
	spot.position = {1, -1, 2};
	spot.direction = {-0.6f, 0.3f, -1};
	spot.range = 5;
	spot.color = {0.2f, 0.3f, 1};
	spot.intensity = 7;
	spot.innerConeRadians = 0.35f;
	spot.outerConeRadians = 0.8f;
	spot.castShadow = true;
	scene.Add(spot);

	// 면적광은 조명을 지원하며 Rect/Disc 광원의 그림자는 현재 지원하지 않는다.
	dyf::RectAreaLight rect;
	rect.position = {0, 1.4f, 2};
	rect.width = 2;
	rect.height = 1;
	rect.intensity = 4;
	rect.color = {1, 0.7f, 0.2f};
	scene.Add(rect);

	dyf::DiscAreaLight disc;
	disc.position = {-1.5f, 0, 2};
	disc.radius = 0.7f;
	disc.intensity = 4;
	disc.color = {0.2f, 1, 0.8f};
	scene.Add(disc);

	dyf::Camera camera;
	dyf::Math::float3 eye = {2.6f, -4, 2.7f}, target = {0, 0, 0};
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
