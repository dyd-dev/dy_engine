// 사용 흐름 설계: 기본 설정을 제공하는 Graphics API의 목표 코드이며 CMake 대상이 아니다.
#include <cstdint>
#include <iostream>
#include <stdexcept>

#include "Platform/Window.h"
#include "Graphics/Mesh.h"
#include "Graphics/Renderer.h"
#include "Graphics/Scene.h"
#include "Math/Math.h"

using namespace dy;

// 목표 API: 색 texture는 sRGB, 데이터 texture는 linear로 구분한다.
// 현재 TextureAsset에는 이 구분이 없다.

namespace
{
	Graphics::TextureAsset MakeSolidTexture(
		uint8_t r,
		uint8_t g,
		uint8_t b,
		Graphics::TextureColorSpace colorSpace,
		uint8_t a = 255u)
	{
		Graphics::TextureAsset texture = {};
		texture.width = 1;
		texture.height = 1;
		texture.colorSpace = colorSpace;
		texture.rgba8 = { r, g, b, a };
		return texture;
	}
}

int main()
{
	try
	{
		Platform::Window window(1280, 720, "Graphics - Materials");
		auto renderer = Graphics::Renderer::Create(window.GetHandle());
		if(!renderer) throw std::runtime_error("Failed to create renderer");

		Graphics::CameraDesc camera = {};
		camera.eye = Math::float3(6.5f, -8.0f, 4.0f);
		camera.target = Math::float3(0.0f, 0.0f, 0.0f);
		camera.fovYRadians = 0.9f;
		camera.aspect = 1280.0f / 720.0f;

		Graphics::PBRDesc pbr = {};
		pbr.minRoughness = 0.04f;
		renderer->SetPBR(pbr);
		Graphics::EnvironmentDesc environment = {};
		environment.diffuseIntensity = 0.3f;
		environment.specularIntensity = 0.8f;
		renderer->SetEnvironmentLight(environment);

		Graphics::Scene scene;
		const MeshID cube = scene.CreateMesh(Graphics::CreateCubeMesh());

		Graphics::TextureAsset checker = {};
		checker.width = 2;
		checker.height = 2;
		checker.colorSpace = Graphics::TextureColorSpace::Srgb;
		checker.rgba8 = {
			235, 235, 235, 255,   30, 110, 220, 255,
			30, 110, 220, 255,   235, 235, 235, 255,
		};
		const TextureID baseColorMap = scene.CreateTexture(checker);
		const TextureID metallicRoughnessMap =
			scene.CreateTexture(MakeSolidTexture(
				255u, 90u, 230u, Graphics::TextureColorSpace::Linear));
		const TextureID normalMap =
			scene.CreateTexture(MakeSolidTexture(
				191u, 128u, 238u, Graphics::TextureColorSpace::Linear));
		const TextureID occlusionMap =
			scene.CreateTexture(MakeSolidTexture(
				190u, 190u, 190u, Graphics::TextureColorSpace::Linear));
		const TextureID emissiveMap =
			scene.CreateTexture(MakeSolidTexture(
				255u, 80u, 20u, Graphics::TextureColorSpace::Srgb));

		Graphics::MaterialDesc matte = {};
		matte.baseColor = Math::float4(0.75f, 0.12f, 0.08f, 1.0f);
		matte.metallicFactor = 0.0f;
		matte.roughnessFactor = 0.9f;

		Graphics::MaterialDesc polishedMetal = {};
		polishedMetal.baseColor = Math::float4(0.75f, 0.78f, 0.82f, 1.0f);
		polishedMetal.metallicFactor = 1.0f;
		polishedMetal.roughnessFactor = 0.12f;

		Graphics::MaterialDesc textured = {};
		textured.baseColorTexture = baseColorMap;
		textured.metallicFactor = 1.0f;
		textured.roughnessFactor = 1.0f;
		textured.metallicRoughnessTexture = metallicRoughnessMap;
		textured.normalTexture = normalMap;
		textured.normalScale = 1.0f;
		textured.occlusionTexture = occlusionMap;
		textured.occlusionStrength = 1.0f;

		Graphics::MaterialDesc emissive = {};
		emissive.baseColor = Math::float4(0.04f, 0.04f, 0.04f, 1.0f);
		emissive.emissiveColor = Math::float3(2.5f, 0.5f, 0.1f);
		emissive.emissiveTexture = emissiveMap;

		const Graphics::MaterialDesc materials[] = { matte, polishedMetal, textured, emissive };
		for(uint32_t i = 0; i < 4; ++i)
		{
			const MaterialID material = scene.CreateMaterial(materials[i]);
			const float x = (static_cast<float>(i) - 1.5f) * 2.0f;
			// 한 mesh와 같은 material을 두 entity가 공유한다.
			for(uint32_t row = 0; row < 2; ++row)
				(void)scene.CreateEntity(cube, material,
					Math::Translation(Math::float3(x, static_cast<float>(row) * 2.0f, 0.0f)));
		}

		Graphics::DirectionalLight light = {};
		light.direction = Math::float3(0.35f, 0.55f, 0.75f);
		light.intensity = 4.0f;
		light.castShadow = false;
		(void)scene.CreateDirectionalLight(light);

		while(window.IsRunning())
		{
			window.PollEvents();
			if(!window.IsRunning()) break;
			if(!renderer->Render(scene, camera))
				throw std::runtime_error("Frame submission failed");
		}
	}
	catch(const std::exception& exception)
	{
		std::cerr << exception.what() << '\n';
		return 1;
	}

	return 0;
}
