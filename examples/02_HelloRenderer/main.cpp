#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include "Platform/Window.h"
#include "RHI/IDevice.h"
#include "Graphics/Renderer.h"
#include "Graphics/Scene.h"
#include "Graphics/Mesh.h"

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
		Platform::Window window(1280, 720, "HelloRenderer");
		std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create(window.GetHandle()));
		if(!device) return -1;

		const std::string ext = ShaderExt();
		const std::string vsPath = std::string(DY_SHADER_DIR) + "/mesh_vs" + ext;
		const std::string psPath = std::string(DY_SHADER_DIR) + "/mesh_ps" + ext;

		Graphics::Renderer renderer;
		Graphics::RendererDesc rendererConfig = {};
		rendererConfig.vertexShaderPath = vsPath.c_str();
		rendererConfig.pixelShaderPath = psPath.c_str();
		if(!renderer.Initialize(device.get(), rendererConfig)) return -1;

		Graphics::Scene scene;

		Graphics::MeshData triangleMesh = {};
		triangleMesh.vertices = {
			Graphics::Vertex{ Math::float3(0.0f, 0.6f, 0.0f), Math::float3(0.0f, 0.0f, 1.0f), Math::float2(0.5f, 0.0f) },
			Graphics::Vertex{ Math::float3(0.6f, -0.6f, 0.0f), Math::float3(0.0f, 0.0f, 1.0f), Math::float2(1.0f, 1.0f) },
			Graphics::Vertex{ Math::float3(-0.6f, -0.6f, 0.0f), Math::float3(0.0f, 0.0f, 1.0f), Math::float2(0.0f, 1.0f) }
		};
		triangleMesh.indices = { 0u, 1u, 2u };

		Graphics::MaterialDesc triangleMaterial = {};

		const MeshID meshId = scene.CreateMesh(triangleMesh);
		const MaterialID materialId = scene.CreateMaterial(triangleMaterial);
		[[maybe_unused]] const EntityID entity = scene.CreateEntity(meshId, materialId);

		while(window.IsRunning())
		{
			window.PollEvents();
			device->BeginFrame();
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
