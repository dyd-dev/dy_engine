// 사용 흐름 설계: 기본 설정을 제공하는 Graphics API의 목표 코드이며 CMake 대상이 아니다.
#include <iostream>
#include <stdexcept>

#include "Platform/Window.h"
#include "Graphics/Mesh.h"
#include "Graphics/Renderer.h"
#include "Graphics/Scene.h"
#include "Math/Math.h"

using namespace dy;

int main(int argc, char** argv)
{
	try
	{
		if(argc > 2) throw std::invalid_argument("Model [model-path]");
		// 동일한 로딩 API로 glTF/GLB, OBJ, FBX를 전달한다. 기본 경로는 저장소 루트 기준이다.
		const char* modelPath = argc > 1
			? argv[1]
			: "examples/Common/Models/DamagedHelmet/glTF/DamagedHelmet.gltf";

		Platform::Window window(1280, 720, "Graphics - Model");
		auto renderer = Graphics::Renderer::Create(window.GetHandle());
		if(!renderer) throw std::runtime_error("Failed to create renderer");

		Graphics::Scene scene;
		Graphics::ModelSceneDesc model = {};
		model.path = modelPath;
		model.normalizedSize = 1.8f;
		if(!Graphics::AddModelToScene(scene, model))
		{
			std::cerr << "Failed to load model: " << modelPath << '\n';
			return 1;
		}

		Graphics::DirectionalLight light = {};
		light.direction = Math::float3(0.35f, 0.55f, 0.75f);
		light.intensity = 4.0f;
		light.castShadow = false;
		(void)scene.CreateDirectionalLight(light);

		Graphics::CameraDesc camera = {};
		camera.eye = Math::float3(3.0f, 3.0f, 2.0f);
		camera.target = Math::float3(0.0f, 0.0f, 0.5f);
		camera.fovYRadians = 1.0f;
		camera.aspect = 1280.0f / 720.0f;
		camera.nearPlane = 0.05f;

		while(window.IsRunning())
		{
			window.PollEvents();
			if(!window.IsRunning()) break;
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
