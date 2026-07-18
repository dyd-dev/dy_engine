#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include "Graphics/Mesh.h"
#include "Graphics/Renderer.h"
#include "Graphics/Scene.h"
#include "Math/Math.h"
#include "Platform/Window.h"
#include "RHI/IDevice.h"

#ifndef DY_SHADER_DIR
#define DY_SHADER_DIR "./Shaders"
#endif

using namespace dy;

namespace
{
	const char* ShaderExt()
	{
#if defined(ENABLE_VULKAN)
		return ".spv";
#else
		return ".glsl";
#endif
	}
}

int main(int argc, char** argv)
{
	try
	{
		uint32_t maxFrames = 0u;
		for(int argumentIndex = 1; argumentIndex < argc; ++argumentIndex)
		{
			const std::string argument = argv[argumentIndex] != nullptr ? argv[argumentIndex] : "";
			const std::string prefix = "--frames=";
			if(argument.rfind(prefix, 0u) == 0u)
			{
				maxFrames = static_cast<uint32_t>(std::strtoul(argument.c_str() + prefix.size(), nullptr, 10));
			}
		}
		Platform::Window window(1280, 720, "LightingLab - Vulkan HDR/PBR");
		RHI::DeviceDesc deviceDesc = {};
		deviceDesc.swapchainFormat = RHI::Format::R8G8B8A8_UNORM_SRGB;
		deviceDesc.maxDrawsPerFrame = 64u;
		std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create(window.GetHandle(), deviceDesc));
		if(!device) throw std::runtime_error("Failed to create Vulkan RHI device");

		const std::string ext = ShaderExt();
		const std::filesystem::path shaderDirectory = argc > 0 && argv[0] != nullptr
			? std::filesystem::absolute(argv[0]).parent_path() / "Shaders"
			: std::filesystem::path(DY_SHADER_DIR);
		const std::string vsPath = (shaderDirectory / ("mesh_vs" + ext)).string();
		const std::string psPath = (shaderDirectory / ("mesh_ps" + ext)).string();
		const std::string shadowVsPath = (shaderDirectory / ("mesh_shadow_vs" + ext)).string();
		const std::string toneVsPath = (shaderDirectory / ("tone_map_vs" + ext)).string();
		const std::string tonePsPath = (shaderDirectory / ("tone_map_ps" + ext)).string();

		Graphics::RendererDesc config = {};
		config.vertexShaderPath = vsPath.c_str();
		config.pixelShaderPath = psPath.c_str();
		config.shadowVertexShaderPath = shadowVsPath.c_str();
		config.toneMapVertexShaderPath = toneVsPath.c_str();
		config.toneMapPixelShaderPath = tonePsPath.c_str();
		config.enableShadows = true;
		config.enableHdrRendering = true;
		config.exposure = 1.0f;
		config.clearColor = Math::float4(0.003f, 0.005f, 0.008f, 1.0f);
		config.ambientIntensity = 0.01f;
		config.shadowCascadeCount = 4u;
		config.shadowCascadeSplitLambda = 0.7f;
		config.shadowMaxFilterRadius = 6.0f;

		Graphics::Renderer renderer;
		if(!renderer.Initialize(device.get(), config)) throw std::runtime_error("Failed to initialize HDR renderer");

		Graphics::CameraDesc camera = {};
		camera.eye = Math::float3(7.0f, -8.0f, 5.5f);
		camera.target = Math::float3(0.0f, 0.0f, 0.4f);
		camera.aspect = 1280.0f / 720.0f;
		camera.farPlane = 80.0f;
		renderer.SetCamera(camera);

		Graphics::Scene scene;
		const MeshID cubeMesh = scene.CreateMesh(Graphics::CreateCubeMesh(1.0f));
		Graphics::MaterialDesc floorMaterial = {};
		floorMaterial.baseColor = Math::float4(0.32f, 0.34f, 0.38f, 1.0f);
		floorMaterial.metallicFactor = 0.0f;
		floorMaterial.roughnessFactor = 0.72f;
		const MaterialID floorMaterialId = scene.CreateMaterial(floorMaterial);
		(void)scene.CreateEntity(
			cubeMesh,
			floorMaterialId,
			Math::Translation(Math::float3(0.0f, 0.0f, -1.0f)) * Math::Scaling(Math::float3(10.0f, 10.0f, 0.2f)));

		for(uint32_t index = 0u; index < 3u; ++index)
		{
			Graphics::MaterialDesc material = {};
			material.baseColor = index == 0u
				? Math::float4(0.82f, 0.18f, 0.08f, 1.0f)
				: (index == 1u ? Math::float4(0.72f, 0.72f, 0.74f, 1.0f) : Math::float4(0.06f, 0.28f, 0.82f, 1.0f));
			material.metallicFactor = index == 1u ? 1.0f : 0.0f;
			material.roughnessFactor = 0.12f + static_cast<float>(index) * 0.32f;
			const MaterialID materialId = scene.CreateMaterial(material);
			(void)scene.CreateEntity(
				cubeMesh,
				materialId,
				Math::Translation(Math::float3(-2.2f + static_cast<float>(index) * 2.2f, 0.0f, 0.0f)));
		}

		Graphics::DirectionalLight sun = {};
		sun.direction = Math::float3(0.45f, 0.65f, 0.62f);
		sun.illuminanceLux = 1.5f;
		sun.angularRadiusRadians = 0.0093f;
		(void)scene.CreateDirectionalLight(sun);

		Graphics::PointLight point = {};
		point.position = Math::float3(0.0f, -2.5f, 2.4f);
		point.color = Math::float3(1.0f, 0.22f, 0.08f);
		point.luminousIntensityCandela = 24.0f;
		point.rangeMeters = 10.0f;
		point.castShadow = false;
		(void)scene.CreatePointLight(point);

		Graphics::SpotLight spot = {};
		spot.position = Math::float3(0.0f, 3.5f, 4.0f);
		spot.direction = Math::float3(0.0f, -0.75f, -1.0f);
		spot.color = Math::float3(0.18f, 0.35f, 1.0f);
		spot.luminousIntensityCandela = 38.0f;
		spot.rangeMeters = 12.0f;
		spot.innerConeRadians = 0.30f;
		spot.outerConeRadians = 0.52f;
		spot.castShadow = false;
		(void)scene.CreateSpotLight(spot);

		Graphics::RectAreaLight rect = {};
		rect.position = Math::float3(-3.2f, -0.5f, 3.2f);
		rect.direction = Math::float3(0.7f, 0.1f, -1.0f);
		rect.up = Math::float3(0.0f, 1.0f, 0.0f);
		rect.color = Math::float3(1.0f, 0.72f, 0.42f);
		rect.luminanceNits = 28.0f;
		rect.widthMeters = 2.4f;
		rect.heightMeters = 1.2f;
		(void)scene.CreateRectAreaLight(rect);

		Graphics::DiscAreaLight disc = {};
		disc.position = Math::float3(3.0f, 0.5f, 3.0f);
		disc.direction = Math::float3(-0.7f, -0.1f, -1.0f);
		disc.color = Math::float3(0.35f, 0.62f, 1.0f);
		disc.luminanceNits = 32.0f;
		disc.radiusMeters = 0.9f;
		(void)scene.CreateDiscAreaLight(disc);

		const auto start = std::chrono::steady_clock::now();
		uint32_t renderedFrames = 0u;
		while(window.IsRunning())
		{
			window.PollEvents();
			const float time = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
			camera.eye = Math::float3(8.5f * std::cos(time * 0.16f), 8.5f * std::sin(time * 0.16f), 5.3f);
			renderer.SetCamera(camera);
			device->BeginFrame();
			renderer.Render(scene, device.get());
			device->Present();
			++renderedFrames;
			if(maxFrames > 0u && renderedFrames >= maxFrames) break;
		}
		const double elapsedMilliseconds = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - start).count();
		if(renderedFrames > 0u)
		{
			std::cout << "LightingLab frames=" << renderedFrames
				<< " average_ms=" << (elapsedMilliseconds / static_cast<double>(renderedFrames)) << '\n';
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
