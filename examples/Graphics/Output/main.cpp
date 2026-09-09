// 사용 흐름 설계: 기본 설정을 제공하는 Graphics API의 목표 코드이며 CMake 대상이 아니다.
// 목표 API: HDR 장면 중간 결과를 현재 SDR backbuffer로 tone mapping한다.
// HDR display 출력은 별도 기능이다.
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>

#include "Platform/Window.h"
#include "Graphics/Mesh.h"
#include "Graphics/Output.h"
#include "Graphics/Renderer.h"
#include "Graphics/Scene.h"
#include "Math/Math.h"

using namespace dy;

int main()
{
	try
	{
		Platform::Window window(1280, 720, "Graphics - Output");
		Graphics::RendererDesc rendererDesc = {};
		rendererDesc.enableHdrRendering = true;
		auto renderer = Graphics::Renderer::Create(window.GetHandle(), rendererDesc);
		if(!renderer) throw std::runtime_error("Failed to create renderer");

		Graphics::CameraDesc camera = {};
		camera.eye = Math::float3(3.0f, 3.0f, 2.0f);
		camera.target = Math::float3(0.0f, 0.0f, 0.0f);
		camera.fovYRadians = 1.0f;
		camera.aspect = 1280.0f / 720.0f;

		Graphics::Scene scene;
		const MeshID cube = scene.CreateMesh(Graphics::CreateCubeMesh());

		Graphics::MaterialDesc brightMaterial = {};
		brightMaterial.baseColor = Math::float4(0.08f, 0.08f, 0.08f, 1.0f);
		brightMaterial.emissiveColor = Math::float3(8.0f, 2.0f, 0.4f);
		const MaterialID material = scene.CreateMaterial(brightMaterial);
		(void)scene.CreateEntity(cube, material);

		Graphics::DirectionalLight light = {};
		light.castShadow = false;
		(void)scene.CreateDirectionalLight(light);

		Graphics::OutputSettings output = {};
		output.toneMapper = Graphics::ToneMapper::ACES;

		const auto start = std::chrono::steady_clock::now();
		while(window.IsRunning())
		{
			window.PollEvents();
			if(!window.IsRunning()) break;
			const float seconds = std::chrono::duration<float>(
				std::chrono::steady_clock::now() - start).count();

			output.exposureEV = 1.5f * std::sin(seconds * 0.5f);
			renderer->SetOutputSettings(output);
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
