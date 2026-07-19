// 03_Cube — CreateCubeMesh + perspective camera + directional light.
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

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
	return ".metallib";
#elif defined(ENABLE_VULKAN)
	return ".spv";
#elif defined(ENABLE_D3D12)
	return ".dxil";
#else
	return ".glsl";
#endif
}

int main()
{
	try
	{
		Platform::Window window(1280, 720, "Cube");
		std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create(window.GetHandle()));
		if(!device) return -1;

		const std::string ext = ShaderExt();
		const std::string vsPath = std::string(DY_SHADER_DIR) + "/mesh_vs" + ext;
		const std::string fragmentShaderPath = std::string(DY_SHADER_DIR) + "/mesh_ps" + ext;

		Graphics::Renderer renderer;
		Graphics::RendererDesc cfg = {};
		cfg.vertexShaderPath = vsPath.c_str();
		cfg.fragmentShaderPath = fragmentShaderPath.c_str();
		if(!renderer.Initialize(device.get(), cfg)) return -1;

		Graphics::CameraDesc camera = {};
		camera.eye = Math::float3(2.5f, 2.5f, 2.5f);
		camera.aspect = 1280.0f / 720.0f;
		renderer.SetCamera(camera);

		Graphics::Scene scene;
		const MeshID cube = scene.CreateMesh(Graphics::CreateCubeMesh(1.0f));
		Graphics::MaterialDesc material = {};
		material.baseColor = Math::float4(0.85f, 0.35f, 0.25f, 1.0f);
		material.roughnessFactor = 0.5f;
		const MaterialID materialId = scene.CreateMaterial(material);
		[[maybe_unused]] const EntityID entity = scene.CreateEntity(cube, materialId);

		Graphics::DirectionalLight light = {};
		light.direction = Math::float3(0.4f, 0.5f, 0.8f);
		light.color = Math::float3(1.0f, 0.96f, 0.9f);
		light.intensity = 3.0f;
		light.castShadow = false;
		[[maybe_unused]] const DirectionalLightID lightId = scene.CreateDirectionalLight(light);

		while(window.IsRunning())
		{
			window.PollEvents();
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
