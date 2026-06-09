// 06_ShadowCube — directional shadow: a cube casts onto a floor.
#include <chrono>
#include <cmath>
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
		Platform::Window window(1280, 720, "ShadowCube");
		std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create(window.GetHandle()));
		if(!device) return -1;

		const std::string ext = ShaderExt();
		const std::string vsPath = std::string(DY_SHADER_DIR) + "/mesh_vs" + ext;
		const std::string psPath = std::string(DY_SHADER_DIR) + "/mesh_ps" + ext;
		const std::string shadowVsPath = std::string(DY_SHADER_DIR) + "/mesh_shadow_vs" + ext;

		Graphics::Renderer renderer;
		Graphics::RendererDesc cfg = {};
		cfg.vertexShaderPath = vsPath.c_str();
		cfg.pixelShaderPath = psPath.c_str();
		cfg.shadowVertexShaderPath = shadowVsPath.c_str();
		cfg.enableShadows = true;
		if(!renderer.Initialize(device.get(), cfg)) return -1;

		Graphics::CameraDesc camera = {};
		camera.eye = Math::float3(5.0f, 5.0f, 4.0f);
		camera.target = Math::float3(0.0f, 0.0f, 0.5f);
		camera.aspect = 1280.0f / 720.0f;
		renderer.SetCamera(camera);

		Graphics::Scene scene;
		const MeshID cube = scene.CreateMesh(Graphics::CreateCubeMesh(1.0f));

		Graphics::MaterialDesc floorMat = {};
		floorMat.baseColor = Math::float4(0.6f, 0.6f, 0.62f, 1.0f);
		const MaterialID floorMatId = scene.CreateMaterial(floorMat);

		Graphics::MaterialDesc cubeMat = {};
		cubeMat.baseColor = Math::float4(0.85f, 0.35f, 0.25f, 1.0f);
		const MaterialID cubeMatId = scene.CreateMaterial(cubeMat);

		// 바닥(넓고 얇은 큐브, 그림자 수신) + 떠 있는 큐브(그림자 생성).
		[[maybe_unused]] const EntityID floorEntity = scene.CreateEntity(
			cube, floorMatId, Math::Translation(Math::float3(0.0f, 0.0f, -1.0f)) * Math::Scaling(Math::float3(8.0f, 8.0f, 0.2f)));
		[[maybe_unused]] const EntityID cubeEntity = scene.CreateEntity(
			cube, cubeMatId, Math::Translation(Math::float3(0.0f, 0.0f, 0.5f)));

		Graphics::DirectionalLight light = {};
		light.direction = Math::float3(0.5f, 0.4f, 0.75f);
		light.color = Math::float3(1.0f, 0.96f, 0.9f);
		light.intensity = 4.0f;
		[[maybe_unused]] const DirectionalLightID lightId = scene.CreateDirectionalLight(light);

		const auto startTime = std::chrono::steady_clock::now();
		while(window.IsRunning())
		{
			window.PollEvents();
			const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - startTime).count();
			const float a = t * 0.4f;

			// 타깃 중심 공전 카메라.
			camera.eye = Math::float3(camera.target.x + 7.0f * std::cos(a), camera.target.y + 7.0f * std::sin(a), camera.target.z + 3.5f);
			renderer.SetCamera(camera);

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
