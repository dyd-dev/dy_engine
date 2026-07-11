// 05_LoadModel - load static glTF/FBX/OBJ models through the shared LoadModel API.
#include <algorithm>
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
#include "LoadModelOptions.h"
#include "LoadModelPlayback.h"
#if defined(ENABLE_VULKAN)
#include "Backends/Vulkan/VulkanDevice.h"
#endif

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
		Examples::LoadModelOptions options;
		std::string optionError;
		if(!Examples::ParseLoadModelOptions(argc, argv, options, optionError))
		{
			std::cerr << optionError << '\n';
			std::cerr << "Usage: LoadModel [model] [--clip=N] [--paused] [--timescale=F] [--loop=0|1] [--smoke-seconds=F]\n";
			return -1;
		}
		Platform::Window window(1280, 720, "LoadModel");
		std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create(window.GetHandle()));
		if(!device) return -1;

		const std::string ext = ShaderExt();
		const std::string vsPath = std::string(DY_SHADER_DIR) + "/mesh_vs" + ext;
		const std::string psPath = std::string(DY_SHADER_DIR) + "/mesh_ps" + ext;
#if defined(ENABLE_VULKAN)
		const std::string computeSkinningPath = std::string(DY_SHADER_DIR) + "/mesh_skinning_cs.spv";
#endif

		Graphics::Renderer renderer;
		Graphics::RendererDesc cfg = {};
		cfg.vertexShaderPath = vsPath.c_str();
		cfg.pixelShaderPath = psPath.c_str();
#if defined(ENABLE_VULKAN)
		cfg.skinningExecutionMode = Graphics::SkinningExecutionMode::ComputePreSkin;
		cfg.computeSkinningShaderPath = computeSkinningPath.c_str();
#endif
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
		std::vector<ModelInstanceID> modelInstances;
		auto addModel = [&scene, &modelInstances](const Graphics::ModelSceneDesc& desc)
		{
			ModelInstanceID instanceId = ModelInstanceID::Invalid;
			const bool loaded = Graphics::AddModelToScene(scene, desc, &instanceId);
			if(loaded && IsValid(instanceId)) modelInstances.push_back(instanceId);
			return loaded;
		};
		if(!options.modelPath.empty())
		{
			Graphics::ModelSceneDesc desc = {};
			desc.path = options.modelPath;
			loadedAny = addModel(desc);
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
				Graphics::ModelSceneDesc desc = {};
				desc.path = models[i];
				desc.position = Math::float3(x, y, 0.5f);
				loadedAny |= addModel(desc);
			}
		}
		if(!loadedAny) return -1;
		Examples::LoadModelPlaybackResult playbackResult;
		std::string playbackError;
		if(!Examples::ConfigureLoadModelAnimations(
			scene,
			modelInstances,
			options,
			std::cout,
			playbackError,
			playbackResult))
		{
			std::cerr << playbackError << '\n';
			return -1;
		}
		std::cout << "Configured animated instances=" << playbackResult.animatedInstances
			<< " static instances=" << playbackResult.staticInstances << '\n';

		Graphics::DirectionalLight light = {};
		light.direction = Math::float3(0.4f, 0.5f, 0.8f);
		light.color = Math::float3(1.0f, 0.96f, 0.9f);
		light.intensity = 3.0f;
		light.castShadow = false;
		[[maybe_unused]] const DirectionalLightID lightId = scene.CreateDirectionalLight(light);

		const auto startTime = std::chrono::steady_clock::now();
		auto previousFrame = startTime;
		while(window.IsRunning())
		{
			window.PollEvents();
			const auto now = std::chrono::steady_clock::now();
			const float deltaSeconds = std::chrono::duration<float>(now - previousFrame).count();
			previousFrame = now;
			const float t = std::chrono::duration<float>(now - startTime).count();

			// 나열된 모델 줄을 중심으로 공전(반경은 줄 길이를 담을 정도).
			const float a = t * 0.4f;
			camera.eye = Math::float3(camera.target.x + 9.0f * std::cos(a), camera.target.y + 9.0f * std::sin(a), camera.target.z + 4.0f);
			renderer.SetCamera(camera);
			scene.UpdateAnimations(deltaSeconds);

			device->BeginFrame();
			renderer.Render(scene, device.get());
			device->Present();
			if(options.smokeSeconds > 0.0f && t >= options.smokeSeconds) break;
		}

		renderer.Shutdown(device.get());
		#if defined(ENABLE_VULKAN)
		const auto* vulkanDevice = dynamic_cast<const Backends::VulkanDevice*>(device.get());
		const bool validationCaptureEnabled = vulkanDevice != nullptr && vulkanDevice->IsValidationCaptureEnabled();
		const uint32_t validationErrorCount = vulkanDevice != nullptr ? vulkanDevice->GetValidationErrorCount() : 0u;
		const uint32_t validationVuidCount = vulkanDevice != nullptr ? vulkanDevice->GetValidationVuidCount() : 0u;
		const bool deviceLost = vulkanDevice != nullptr && vulkanDevice->IsDeviceLost();
		std::cout << "VULKAN_VALIDATION_CAPTURE_ENABLED=" << (validationCaptureEnabled ? 1 : 0) << '\n';
		std::cout << "VULKAN_VALIDATION_ERROR_COUNT=" << validationErrorCount << '\n';
		std::cout << "VULKAN_VALIDATION_VUID_COUNT=" << validationVuidCount << '\n';
		std::cout << "VULKAN_DEVICE_LOST=" << (deviceLost ? 1 : 0) << '\n';
		#if !defined(NDEBUG)
		if(!validationCaptureEnabled) return -3;
		#endif
		if(validationErrorCount != 0u) return -2;
		if(validationVuidCount != 0u) return -4;
		if(deviceLost) return -4;
		#endif
		return 0;
	}
	catch(const std::exception& exception)
	{
		std::cerr << exception.what() << '\n';
		return -1;
	}
}
