// 사용 흐름 설계: 기본 설정을 제공하는 Graphics API의 목표 코드이며 CMake 대상이 아니다.
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>

#include "Platform/Window.h"
#include "Graphics/Mesh.h"
#include "Graphics/Renderer.h"
#include "Graphics/Scene.h"
#include "Math/Math.h"

using namespace dy;

// 목표 동작: 공개된 모든 광원이 같은 frame에 기여하며 point light가 있어도
// directional shadow가 유지된다. main의 다중 광원·그림자 기능을 축소하지 않는다.

int main()
{
	try
	{
		Platform::Window window(1280, 720, "Graphics - Lighting");

		Graphics::RendererDesc rendererDesc = {};
		rendererDesc.enableShadows = true;
		auto renderer = Graphics::Renderer::Create(window.GetHandle(), rendererDesc);
		if(!renderer) throw std::runtime_error("Failed to create renderer");

		Graphics::CameraDesc camera = {};
		camera.eye = Math::float3(7.0f, -8.0f, 5.0f);
		camera.target = Math::float3(0.0f, 0.0f, 0.5f);
		camera.fovYRadians = 0.9f;
		camera.aspect = 1280.0f / 720.0f;

		Graphics::Scene scene;
		const MeshID cube = scene.CreateMesh(Graphics::CreateCubeMesh());

		Graphics::MaterialDesc floorMaterial = {};
		floorMaterial.baseColor = Math::float4(0.55f, 0.57f, 0.60f, 1.0f);
		floorMaterial.roughnessFactor = 0.8f;
		const MaterialID floorMaterialId = scene.CreateMaterial(floorMaterial);

		Graphics::MaterialDesc objectMaterial = {};
		objectMaterial.baseColor = Math::float4(0.75f, 0.72f, 0.66f, 1.0f);
		objectMaterial.roughnessFactor = 0.35f;
		const MaterialID objectMaterialId = scene.CreateMaterial(objectMaterial);

		Graphics::RenderFlags floorFlags = {};
		floorFlags.castShadow = false;
		floorFlags.receiveShadow = true;
		(void)scene.CreateEntity(
			cube,
			floorMaterialId,
			Math::Translation(Math::float3(0.0f, 0.0f, -0.7f)) *
				Math::Scaling(Math::float3(10.0f, 8.0f, 0.2f)),
			floorFlags);

		Graphics::RenderFlags objectFlags = {};
		objectFlags.castShadow = true;
		objectFlags.receiveShadow = true;
		(void)scene.CreateEntity(
			cube,
			objectMaterialId,
			Math::Translation(Math::float3(-1.8f, 0.0f, 0.0f)),
			objectFlags);
		(void)scene.CreateEntity(
			cube,
			objectMaterialId,
			Math::Translation(Math::float3(1.8f, 0.0f, 0.8f)),
			objectFlags);

		Graphics::DirectionalLight sun = {};
		sun.direction = Math::float3(0.45f, 0.65f, 0.62f);
		sun.intensity = 1.5f;
		sun.castShadow = true;
		(void)scene.CreateDirectionalLight(sun);

		Graphics::PointLight point = {};
		point.position = Math::float3(0.0f, -2.5f, 2.4f);
		point.color = Math::float3(1.0f, 0.22f, 0.08f);
		point.intensity = 24.0f;
		point.range = 10.0f;
		point.castShadow = true;
		const PointLightID movingPoint = scene.CreatePointLight(point);

		// 같은 종류의 광원 여러 개를 한 장면에 함께 등록한다.
		Graphics::PointLight coolPoint = {};
		coolPoint.position = Math::float3(1.6f, -1.8f, 2.0f);
		coolPoint.color = Math::float3(0.2f, 0.45f, 1.0f);
		coolPoint.intensity = 18.0f;
		coolPoint.range = 10.0f;
		coolPoint.castShadow = false;
		(void)scene.CreatePointLight(coolPoint);

		Graphics::SpotLight spot = {};
		spot.position = Math::float3(0.0f, 3.5f, 4.0f);
		spot.direction = Math::float3(0.0f, -0.75f, -1.0f);
		spot.color = Math::float3(0.18f, 0.35f, 1.0f);
		spot.intensity = 38.0f;
		spot.range = 12.0f;
		spot.innerConeRadians = 0.30f;
		spot.outerConeRadians = 0.52f;
		spot.castShadow = true;
		(void)scene.CreateSpotLight(spot);

		Graphics::RectAreaLight rect = {};
		rect.position = Math::float3(-3.2f, -0.5f, 3.2f);
		rect.direction = Math::float3(0.7f, 0.1f, -1.0f);
		rect.up = Math::float3(0.0f, 1.0f, 0.0f);
		rect.color = Math::float3(1.0f, 0.72f, 0.42f);
		rect.intensity = 28.0f;
		rect.width = 2.4f;
		rect.height = 1.2f;
		(void)scene.CreateRectAreaLight(rect);

		Graphics::DiscAreaLight disc = {};
		disc.position = Math::float3(3.0f, 0.5f, 3.0f);
		disc.direction = Math::float3(-0.7f, -0.1f, -1.0f);
		disc.color = Math::float3(0.35f, 0.62f, 1.0f);
		disc.intensity = 32.0f;
		disc.radius = 0.9f;
		(void)scene.CreateDiscAreaLight(disc);

		const auto start = std::chrono::steady_clock::now();
		while(window.IsRunning())
		{
			window.PollEvents();
			if(!window.IsRunning()) break;
			const float seconds = std::chrono::duration<float>(
				std::chrono::steady_clock::now() - start).count();
			point.position.x = 2.5f * std::sin(seconds * 0.7f);
			scene.SetPointLight(ToIndex(movingPoint), point);
			if(!renderer->Render(scene, camera))
				throw std::runtime_error("Frame submission failed");
		}
	}
	catch(const std::exception& exception)
	{
		std::cerr << exception.what() << '\n';
		return 1;
	}

	return 0;
}
