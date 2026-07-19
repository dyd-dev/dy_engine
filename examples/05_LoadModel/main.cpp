// 05_LoadModel - load static glTF/FBX/OBJ models through the shared LoadModel API.
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "Platform/Window.h"
#include "RHI/IDevice.h"
#include "Graphics/Renderer.h"
#include "Graphics/Scene.h"
#include "Graphics/Mesh.h"
#include "Math/Math.h"

#ifndef DY_SHADER_DIR
#define DY_SHADER_DIR "./Shaders"
#endif

using namespace dy;

static const char* ShaderExt()
{
#if defined(ENABLE_METAL)
	return ".metal";
#elif defined(ENABLE_VULKAN)
	return ".spv";
#elif defined(ENABLE_D3D12)
	return ".hlsl";
#else
	return ".glsl";
#endif
}

int main(int argc, char** argv)
{
	try
	{
		Platform::Window window(1280, 720, "LoadModel");
		std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create(window.GetHandle()));
		if(!device) return -1;

		const std::string ext = ShaderExt();
		const std::string vsPath = std::string(DY_SHADER_DIR) + "/mesh_vs" + ext;
		const std::string psPath = std::string(DY_SHADER_DIR) + "/mesh_ps" + ext;

		Graphics::Renderer renderer;
		Graphics::RendererDesc cfg = {};
		cfg.vertexShaderPath = vsPath.c_str();
		cfg.pixelShaderPath = psPath.c_str();
		if(!renderer.Initialize(device.get(), cfg)) return -1;

		Graphics::CameraDesc camera = {};
		camera.eye = Math::float3(3.0f, 3.0f, 2.0f);
		camera.target = Math::float3(0.0f, 0.0f, 0.5f);
		camera.aspect = 1280.0f / 720.0f;
		camera.nearPlane = 0.05f;
		camera.farPlane = 200.0f;
		renderer.SetCamera(camera);

		Graphics::Scene scene;

		bool loadedAny = false;
		if(argc > 1)
		{
			loadedAny = Graphics::AddModelToScene(scene, argv[1]);
		}
		else
		{
			const std::vector<const char*> models = {
				"Models/Duck/glTF/Duck.gltf",
				"Models/Avocado/glTF/Avocado.gltf",
				"Models/BoomBox/glTF/BoomBox.gltf",
				"Models/DamagedHelmet/glTF/DamagedHelmet.gltf",
				"Models/WaterBottle/glTF/WaterBottle.gltf",
				"Models/Lowpoly_tree/Lowpoly_tree.obj",
				"Models/shiba/scene.FBX",
			};
			const float spacing = 2.3f;
			const int columnCount = 4;
			for(size_t i = 0; i < models.size(); ++i)
			{
				const int column = static_cast<int>(i % columnCount);
				const int row = static_cast<int>(i / columnCount);
				const float x = (static_cast<float>(column) - 0.5f * static_cast<float>(columnCount - 1)) * spacing;
				const float y = (0.5f - static_cast<float>(row)) * spacing;
				loadedAny |= Graphics::AddModelToScene(scene, models[i], Math::float3(x, y, 0.5f));
			}
		}
		if(!loadedAny) return -1;

		Graphics::DirectionalLight light = {};
		light.direction = Math::float3(0.4f, 0.5f, 0.8f);
		light.color = Math::float3(1.0f, 0.96f, 0.9f);
		light.intensity = 3.0f;
		light.castShadow = false;
		[[maybe_unused]] const DirectionalLightID lightId = scene.CreateDirectionalLight(light);

		const auto startTime = std::chrono::steady_clock::now();
		while(window.IsRunning())
		{
			window.PollEvents();
			const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - startTime).count();

			// 나열된 모델 줄을 중심으로 공전(반경은 줄 길이를 담을 정도).
			const float a = t * 0.4f;
			camera.eye = Math::float3(camera.target.x + 9.0f * std::cos(a), camera.target.y + 9.0f * std::sin(a), camera.target.z + 4.0f);
			renderer.SetCamera(camera);

			if(!device->BeginFrame()) continue;
			renderer.Render(scene, device.get());
			device->Present();
		}

		renderer.Shutdown(device.get());
		return 0;
	}
	catch(const std::exception& exception)
	{
		std::cerr << exception.what() << '\n';
		return -1;
	}
}
