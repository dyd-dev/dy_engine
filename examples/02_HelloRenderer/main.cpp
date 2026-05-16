#include <iostream>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

#include "Platform/Window.h"
#include "RHI/IDevice.h"
#include "Graphics/Renderer.h"

#ifndef DY_SHADER_DIR
#define DY_SHADER_DIR "./Shader"
#endif

using namespace dy;

const char* GetShaderExtension();
std::string ReadTextFile(const char* filepath);

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
		const std::string vertexShader = ReadTextFile(vertexShaderPath.c_str());
		const std::string pixelShader = ReadTextFile(pixelShaderPath.c_str());

		RHI::GraphicsPipelineDesc mainPipeline = {};
		mainPipeline.vertexShader = vertexShader.data();
		mainPipeline.vertexShaderSize = vertexShader.size();
		mainPipeline.pixelShader = pixelShader.data();
		mainPipeline.pixelShaderSize = pixelShader.size();
		mainPipeline.renderTargetFormat = RHI::Format::R8G8B8A8_UNORM;
		if(!renderer.Initialize(device.get(), mainPipeline, rendererConfig)) return -1;

		Graphics::Scene scene;

		Graphics::MeshData triangleMesh = {};
		triangleMesh.vertices = {
			Graphics::Vertex{ Math::float3(0.0f, 0.6f, 0.0f), Math::float3(0.0f, 0.0f, 1.0f), Math::float2(0.5f, 0.0f) },
			Graphics::Vertex{ Math::float3(0.6f, -0.6f, 0.0f), Math::float3(0.0f, 0.0f, 1.0f), Math::float2(1.0f, 1.0f) },
			Graphics::Vertex{ Math::float3(-0.6f, -0.6f, 0.0f), Math::float3(0.0f, 0.0f, 1.0f), Math::float2(0.0f, 1.0f) }
		};
		triangleMesh.indices = { 0u, 1u, 2u };

		Graphics::MaterialData triangleMaterial = {};
		triangleMaterial.baseColor = Math::float4(1.0f, 1.0f, 1.0f, 1.0f);
		triangleMaterial.baseColorTex = static_cast<TextureID>(0u);

		scene.m_meshes.push_back(triangleMesh);
		scene.m_materials.push_back(triangleMaterial);
		scene.m_entityMeshes.push_back(static_cast<MeshID>(0u));
		scene.m_entityMaterials.push_back(static_cast<MaterialID>(0u));
		scene.m_entityTransforms.push_back(Graphics::Transform{ Math::float4x4::Identity() });

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

std::string ReadTextFile(const char* filepath)
{
	std::ifstream file(filepath, std::ios::binary);
	if(!file.is_open())
	{
		throw std::runtime_error(std::string("Failed to open shader file: ") + filepath);
	}

	file.seekg(0, std::ios::end);
	const std::streamoff size = file.tellg();
	file.seekg(0, std::ios::beg);

	std::string content(static_cast<size_t>(size), '\0');
	if(size > 0)
	{
		file.read(content.data(), size);
	}
	return content;
}
