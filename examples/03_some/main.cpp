#include <iostream>
#include <memory>
#include <string>

#include "Platform/Window.h"
#include "RHI/IDevice.h"
#include "Graphics/Loaders.h"
#include "Graphics/Renderer.h"

#ifndef DY_SHADER_DIR
#define DY_SHADER_DIR "./Shader"
#endif

#ifndef DY_MODEL_PATH
#define DY_MODEL_PATH "./Model/Lowpoly_tree.obj"
#endif

using namespace dy;

const char* GetShaderExtension();

int main(int argc, char** argv)
{
	try
	{
		const char* objPath = argc > 1 ? argv[1] : DY_MODEL_PATH;

		Graphics::MeshData mesh = {};
		if(!Graphics::LoadFromOBJ(objPath, mesh))
		{
			std::cerr << "Failed to load OBJ: " << objPath << '\n';
			return -1;
		}

		Platform::Window window(1280, 720, "Vulkan OBJ Shadow");
		std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create(window.GetHandle()));
		if(!device) return -1;

		Graphics::Renderer renderer;
		Graphics::RendererConfig rendererConfig = {};
		rendererConfig.clearColor = Math::float4(0.02f, 0.025f, 0.035f, 1.0f);

		const std::string shaderExtension = GetShaderExtension();
		const std::string vertexShaderPath = std::string(DY_SHADER_DIR) + "/TexturedTriangleVS" + shaderExtension;
		const std::string pixelShaderPath = std::string(DY_SHADER_DIR) + "/TexturedTrianglePS" + shaderExtension;

		Graphics::GraphicsPipelineFiles mainPipeline = {};
		mainPipeline.vertexShaderPath = vertexShaderPath.c_str();
		mainPipeline.pixelShaderPath = pixelShaderPath.c_str();
		mainPipeline.depthStencilFormat = RHI::Format::D32_FLOAT;
		mainPipeline.depthTestEnable = true;
		mainPipeline.depthWriteEnable = true;

		if(!renderer.Initialize(device.get(), mainPipeline, rendererConfig)) return -1;

		// Future Vulkan shadow/light merge point.
		// These names intentionally describe the Renderer-facing API this example wants.
		// Current branch does not define these types/functions yet; compile errors here are expected.
#if defined(ENABLE_VULKAN)
		Graphics::DirectionalLight directionalLight = {};
		directionalLight.direction = Math::float3(-0.35f, -1.0f, -0.25f);
		directionalLight.color = Math::float3(1.0f, 0.96f, 0.88f);
		directionalLight.intensity = 3.0f;

		Graphics::ShadowMapDesc shadowMap = {};
		shadowMap.resolution = 2048;
		shadowMap.depthFormat = RHI::Format::D32_FLOAT;
		shadowMap.lightView = Math::float4x4::Identity();
		shadowMap.lightProjection = Math::float4x4::Identity();

		renderer.SetDirectionalLight(directionalLight);
		renderer.SetAmbientLight(Math::float3(0.04f, 0.045f, 0.055f));
		renderer.EnableShadowMap(device.get(), shadowMap);
#endif

		Graphics::Scene scene;

		Graphics::MaterialData material = {};
		material.baseColor = Math::float4(0.72f, 0.78f, 0.64f, 1.0f);
		material.baseColorTex = TextureID::Invalid;

		scene.m_meshes.push_back(mesh);
		scene.m_materials.push_back(material);
		scene.m_entityMeshes.push_back(static_cast<MeshID>(0u));
		scene.m_entityMaterials.push_back(static_cast<MaterialID>(0u));
		scene.m_entityTransforms.push_back(Graphics::Transform{ Math::float4x4::Identity() });

		while(window.IsRunning())
		{
			window.PollEvents();

			device->BeginFrame();

			// Future Renderer merge point. The current Render(scene, device) path draws
			// directly; a shadow-aware renderer would record shadow/depth first, then main.
#if defined(ENABLE_VULKAN)
			renderer.RenderShadowMap(scene, device.get());
#endif
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
