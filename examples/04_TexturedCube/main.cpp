// 04_TexturedCube — cube with a base-color texture (decoded at upload via stb).
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
	return ".metal";
#elif defined(ENABLE_VULKAN)
	return ".spv";
#elif defined(ENABLE_D3D12)
	return ".hlsl";
#else
	return ".glsl";
#endif
}

int main()
{
	try
	{
		Platform::Window window(1280, 720, "TexturedCube");
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
		camera.eye = Math::float3(2.5f, 2.5f, 2.0f);
		camera.aspect = 1280.0f / 720.0f;
		renderer.SetCamera(camera);

		Graphics::Scene scene;
		const MeshID cube = scene.CreateMesh(Graphics::CreateCubeMesh(1.0f));

		// Scene 는 경로만 보관; 실제 디코드/업로드는 렌더러(GpuScene)가 수행한다.
		const TextureID texture = scene.CreateTexture(std::string("Textures/cube.png"));
		Graphics::MaterialDesc material = {};
		material.baseColorTexture = texture;
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
