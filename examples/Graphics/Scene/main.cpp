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

int main()
{
	try
	{
		Platform::Window window(1280, 720, "Graphics - Scene");
		auto renderer = Graphics::Renderer::Create(window.GetHandle());
		if(!renderer) throw std::runtime_error("Failed to create renderer");

		Graphics::CameraDesc camera = {};
		camera.eye = Math::float3(3.0f, 3.0f, 2.2f);
		camera.target = Math::float3(0.0f, 0.0f, 0.0f);
		camera.aspect = 1280.0f / 720.0f;

		Graphics::Scene scene;
		const MeshID mesh = scene.CreateMesh(Graphics::CreateCubeMesh());

		Graphics::MaterialDesc material = {};
		material.baseColor = Math::float4(0.85f, 0.30f, 0.18f, 1.0f);
		material.roughnessFactor = 0.45f;
		const MaterialID materialId = scene.CreateMaterial(material);
		const EntityID cube = scene.CreateEntity(mesh, materialId);

		Graphics::DirectionalLight light = {};
		light.direction = Math::float3(0.4f, 0.6f, 0.8f);
		light.castShadow = false;
		(void)scene.CreateDirectionalLight(light);

		Math::Bounds3 debugBounds = {};
		debugBounds.Include(Math::float3(-0.75f, -0.75f, -0.75f));
		debugBounds.Include(Math::float3(0.75f, 0.75f, 0.75f));

		const auto start = std::chrono::steady_clock::now();
		while(window.IsRunning())
		{
			window.PollEvents();
			if(!window.IsRunning()) break;
			const float seconds = std::chrono::duration<float>(
				std::chrono::steady_clock::now() - start).count();

			scene.GetTransform(cube).worldMatrix =
				Math::Translation(Math::float3(0.0f, 0.0f, 0.2f * std::sin(seconds))) *
				Math::RotationZ(seconds * 0.7f) *
				Math::RotationX(seconds * 0.4f);

			// 제안 API: 월드 좌표의 점·선과 XY 평면 사각형을 이번 frame에 그린다.
			// 색·두께 기본값과 필요한 삼각형 확장은 Graphics가 정한다.
			renderer->DrawPoint(Math::float3(0.0f, 0.0f, 1.5f));
			renderer->DrawLine(Math::float3(-1.0f, 0.0f, 0.0f), Math::float3(1.0f, 0.0f, 0.0f));
			renderer->DrawRect(Math::float3(-1.0f, -1.0f, 0.0f), Math::float2(2.0f, 2.0f));
			renderer->DrawBox(debugBounds);
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
