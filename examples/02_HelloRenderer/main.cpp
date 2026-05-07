#include <array>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "Platform/Window.h"
#include "RHI/IDevice.h"
#include "Graphics/Renderer.h"
#include "Graphics/Scene.h"
#include "Core/Image.h"

#ifndef DY_SHADER_DIR
#define DY_SHADER_DIR "./Shader"
#endif

using namespace dy;

Core::Image CreateCheckerboardImage(
	uint32_t width, uint32_t height, uint32_t cellSize,
	const std::array<uint8_t, 4>& evenColor, const std::array<uint8_t, 4>& oddColor);

const char* GetShaderExtension();

int main()
{
	try
	{
		Platform::Window window(1280, 720, "HelloRenderer");
		std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create(window.GetHandle()));
		if(!device) return -1;

		Graphics::Renderer renderer;
		Graphics::RendererConfig rendererConfig = {};
		const std::string shaderExtension = GetShaderExtension();
		const std::string vertexShaderPath = std::string(DY_SHADER_DIR) + "/TexturedTriangleVS" + shaderExtension;
		const std::string pixelShaderPath = std::string(DY_SHADER_DIR) + "/TexturedTrianglePS" + shaderExtension;
		rendererConfig.vertexShaderPath = vertexShaderPath.c_str();
		rendererConfig.pixelShaderPath = pixelShaderPath.c_str();
		if(!renderer.Initialize(device.get(), rendererConfig)) return -1;

		Graphics::Scene scene;

		Core::Image textureImage = CreateCheckerboardImage(
			256, 256, 32,
			{ 240, 240, 240, 255 },
			{ 64, 160, 216, 255 }
		);

		const TextureID checkerTexture = scene.CreateTexture(textureImage);
		const MaterialID triangleMaterial = scene.CreateMaterial(Material{
			Math::float4(1.0f, 1.0f, 1.0f, 1.0f),
			checkerTexture
		});
		Mesh triangleMeshData = {};
		triangleMeshData.vertices = {
			Vertex{ 0.0f, 0.6f, 0.0f, 0.5f, 0.0f },
			Vertex{ 0.6f, -0.6f, 0.0f, 1.0f, 1.0f },
			Vertex{ -0.6f, -0.6f, 0.0f, 0.0f, 1.0f }
		};
		triangleMeshData.indices = { 0u, 1u, 2u };
		const MeshID triangleMesh = scene.CreateMesh(triangleMeshData);
		[[maybe_unused]]
		const EntityID triangleEntity = scene.CreateEntity(triangleMesh, triangleMaterial);

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

Core::Image CreateCheckerboardImage(
	uint32_t width,
	uint32_t height,
	uint32_t cellSize,
	const std::array<uint8_t, 4>& evenColor,
	const std::array<uint8_t, 4>& oddColor)
{
	std::vector<uint8_t> pixels(width * height * 4u, 0u);

	for(uint32_t y = 0; y < height; ++y)
	{
		for(uint32_t x = 0; x < width; ++x)
		{
			const bool isEvenCell = (((x / cellSize) + (y / cellSize)) & 1u) == 0u;
			const auto& color = isEvenCell ? evenColor : oddColor;
			const uint32_t pixelIndex = (y * width + x) * 4u;

			pixels[pixelIndex + 0u] = color[0];
			pixels[pixelIndex + 1u] = color[1];
			pixels[pixelIndex + 2u] = color[2];
			pixels[pixelIndex + 3u] = color[3];
		}
	}

	return Core::Image(width, height, std::move(pixels));
}

const char* GetShaderExtension()
{
#if defined(ENABLE_METAL)
	return ".metal";
#elif defined(ENABLE_VULKAN)
	return ".glsl";
#else
	return ".hlsl";
#endif
}
