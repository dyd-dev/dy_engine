#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

#include "Core/Types.h"
#include "Graphics/AssetManager.h"
#include "Graphics/Material.h"
#include "Graphics/Renderer.h"
#include "Graphics/Scene.h"
#include "Math/Math.h"
#include "Platform/Window.h"
#include "RHI/IDevice.h"

using namespace dy;

namespace
{
	constexpr uint32_t kObjectCount = 1000;
	constexpr float kTwoPi = 6.28318530718f;
	constexpr float kMinOrbitRadius = 4.0f;
	constexpr float kMaxOrbitRadius = 65.0f;
	constexpr float kHalfHeightRange = 65.0f;

	struct OrbitObject
	{
		EntityID entity = EntityID::Invalid;
		float radius = 1.0f;
		float angle = 0.0f;
		float speed = 0.1f;
		float height = 0.0f;
		float scale = 0.05f;
		float spin = 0.0f;
	};

	uint32_t HashUInt(uint32_t value)
	{
		value ^= value >> 16u;
		value *= 0x7feb352du;
		value ^= value >> 15u;
		value *= 0x846ca68bu;
		value ^= value >> 16u;
		return value;
	}

	float Random01(uint32_t index, uint32_t salt)
	{
		const uint32_t value = HashUInt(index ^ (salt * 0x9e3779b9u));
		return static_cast<float>(value & 0x00ffffffu) / static_cast<float>(0x00ffffffu);
	}

	float Lerp(float a, float b, float t)
	{
		return a + (b - a) * t;
	}

	Math::float4x4 CreateViewProjection()
	{
		const Math::float3 eye(62.0f, -74.0f, 52.0f);
		const Math::float3 target(0.0f, 0.0f, 0.0f);
		const Math::float4x4 view = Math::LookAtRH(eye, target, Math::float3(0.0f, 0.0f, 1.0f));
		const Math::float4x4 projection = Math::OrthographicRH_ZO(84.0f, 54.0f, 0.1f, 240.0f);
		return Math::MultiplyColumnMajor(projection, view);
	}

	Math::float4x4 CreateOrbitModel(const OrbitObject& object, float seconds)
	{
		const float angle = object.angle + seconds * object.speed;
		const float x = std::cos(angle) * object.radius;
		const float y = std::sin(angle) * object.radius;
		const float z = object.height;
		const float yaw = angle + seconds * object.spin;
		const float cosYaw = std::cos(yaw);
		const float sinYaw = std::sin(yaw);

		Math::float4x4 model = Math::float4x4::Identity();
		model.m[0] = object.scale * cosYaw;
		model.m[1] = object.scale * sinYaw;
		model.m[4] = object.scale * -sinYaw;
		model.m[5] = object.scale * cosYaw;
		model.m[10] = object.scale;
		model.m[12] = x;
		model.m[13] = y;
		model.m[14] = z;
		return model;
	}

	Vertex CreateSimpleVertex(float x, float y, float z, float u, float v)
	{
		Vertex vertex = {};
		vertex.px = x;
		vertex.py = y;
		vertex.pz = z;
		vertex.u = u;
		vertex.v = v;
		vertex.nx = 0.0f;
		vertex.ny = 0.0f;
		vertex.nz = 1.0f;
		vertex.tx = 1.0f;
		vertex.ty = 0.0f;
		vertex.tz = 0.0f;
		vertex.tw = 1.0f;
		return vertex;
	}

	Mesh CreateLightMarkerMesh()
	{
		Mesh mesh = {};
		const float s = 1.12f;
		mesh.vertices = {
			CreateSimpleVertex(0.0f, 0.0f, s, 0.5f, 1.0f),
			CreateSimpleVertex(0.0f, 0.0f, -s, 0.5f, 0.0f),
			CreateSimpleVertex(s, 0.0f, 0.0f, 1.0f, 0.5f),
			CreateSimpleVertex(-s, 0.0f, 0.0f, 0.0f, 0.5f),
			CreateSimpleVertex(0.0f, s, 0.0f, 0.5f, 0.5f),
			CreateSimpleVertex(0.0f, -s, 0.0f, 0.5f, 0.5f)
		};
		mesh.indices = {
			0, 2, 4, 0, 4, 3, 0, 3, 5, 0, 5, 2,
			1, 4, 2, 1, 3, 4, 1, 5, 3, 1, 2, 5
		};
		return mesh;
	}

	Math::float4x4 CreateTranslation(const Math::float3& position)
	{
		Math::float4x4 matrix = Math::float4x4::Identity();
		matrix.m[12] = position.x;
		matrix.m[13] = position.y;
		matrix.m[14] = position.z;
		return matrix;
	}

	Graphics::MaterialDesc CreateOrbitMaterial(uint32_t index)
	{
		const float t0 = static_cast<float>((index * 37u) % 100u) / 100.0f;
		const float t1 = static_cast<float>((index * 59u) % 100u) / 100.0f;
		Graphics::MaterialDesc material = {};
		material.baseColor = Math::float4(0.35f + t0 * 0.55f, 0.42f + t1 * 0.38f, 0.85f - t0 * 0.35f, 1.0f);
		material.metallicFactor = 0.15f + t1 * 0.55f;
		material.roughnessFactor = 0.28f + t0 * 0.45f;
		material.normalScale = 0.12f;
		return material;
	}
}

int main()
{
	try
	{
		Platform::Window window(1280, 720, "Orbit Scene");

		RHI::DeviceDesc deviceDesc = {};
		deviceDesc.maxDrawsPerFrame = kObjectCount + 1u;
		std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create(window.GetHandle(), deviceDesc));
		if(!device)
		{
			throw std::runtime_error("Failed to initialize RHI device for orbit lit scene");
		}

		Graphics::Scene scene;
		Graphics::AssetManager assets(scene);

		const Math::float3 lightPosition(0.0f, 0.0f, 0.0f);
		[[maybe_unused]] const uint32_t centerLight = scene.CreatePointLight(
			lightPosition,
			145.0f,
			Math::float3(1.0f, 0.88f, 0.62f),
			1400.0f,
			Math::float3(0.0f, 0.0f, -1.0f),
			true,
			0.62f);

		Graphics::MaterialDesc lightMaterial = {};
		lightMaterial.baseColor = Math::float4(1.0f, 0.88f, 0.42f, 1.0f);
		lightMaterial.emissiveColor = Math::float3(1.0f, 0.72f, 0.18f);
		lightMaterial.roughnessFactor = 0.18f;
		const MeshID lightMesh = scene.CreateMesh(CreateLightMarkerMesh());
		const MaterialID lightMaterialId = scene.CreateMaterial(lightMaterial);
		RenderFlags lightFlags = {};
		lightFlags.castShadow = false;
		lightFlags.receiveShadow = false;
		[[maybe_unused]] const EntityID lightEntity = scene.CreateEntity(lightMesh, lightMaterialId, CreateTranslation(lightPosition), lightFlags);

		std::vector<OrbitObject> orbitObjects;
		orbitObjects.reserve(kObjectCount);
		RenderFlags objectFlags = {};
		objectFlags.castShadow = true;
		objectFlags.receiveShadow = true;
		for(uint32_t index = 0; index < kObjectCount; ++index)
		{
			OrbitObject object = {};
			const float radiusT = std::sqrt(Random01(index, 11u));
			const float spinDirection = Random01(index, 43u) < 0.5f ? -1.0f : 1.0f;
			object.radius = Lerp(kMinOrbitRadius, kMaxOrbitRadius, radiusT);
			object.angle = Random01(index, 17u) * kTwoPi;
			object.speed = spinDirection * Lerp(0.045f, 0.23f, Random01(index, 29u));
			object.height = Lerp(-kHalfHeightRange, kHalfHeightRange, Random01(index, 37u));
			object.scale = Lerp(0.26f, 0.58f, Random01(index, 53u));
			object.spin = Lerp(0.18f, 0.72f, Random01(index, 61u));

			const Graphics::ObjectLoadResult result = assets.LoadObject(
				"examples/vulkan_test/triangle.obj",
				CreateOrbitMaterial(index),
				CreateOrbitModel(object, 0.0f),
				objectFlags);
			if(!result)
			{
				throw std::runtime_error("Could not load orbit object mesh");
			}

			object.entity = result.entity;
			orbitObjects.push_back(object);
		}

		Graphics::RendererDesc rendererConfig = {};
		rendererConfig.viewProjectionMatrix = CreateViewProjection();
		rendererConfig.cameraPosition = Math::float3(62.0f, -74.0f, 52.0f);
		rendererConfig.clearColor = Math::float4(0.015f, 0.018f, 0.030f, 1.0f);
		rendererConfig.ambientColor = Math::float3(0.46f, 0.50f, 0.62f);
		rendererConfig.ambientIntensity = 0.035f;
		rendererConfig.pbr.minRoughness = 0.04f;
		rendererConfig.pbr.ambientSpecularStrength = 0.26f;
		rendererConfig.environment.diffuseColor = Math::float3(0.48f, 0.52f, 0.66f);
		rendererConfig.environment.specularColor = Math::float3(0.70f, 0.78f, 1.0f);
		rendererConfig.environment.specularIntensity = 0.55f;
		rendererConfig.enableShadows = true;
		rendererConfig.shadowMap.resolution = 2048;
		rendererConfig.shadowMap.nearPlane = 0.1f;
		rendererConfig.shadowMap.farPlane = 220.0f;
		rendererConfig.shadowMap.spotFovYRadians = 2.85f;
		rendererConfig.shadowMap.sceneCenter = Math::float3(0.0f, 0.0f, 0.0f);
		rendererConfig.autoFitShadowMap = false;
		rendererConfig.shadowDepthBias = 0.0008f;
		rendererConfig.shadowSlopeBias = 0.0035f;
		rendererConfig.shadowNormalBias = 0.00025f;
		rendererConfig.shadowPcfRadius = 1;

		Graphics::Renderer renderer;
		if(!renderer.Initialize(device.get(), rendererConfig))
		{
			throw std::runtime_error("Failed to initialize renderer for orbit lit scene");
		}

		const auto startTime = std::chrono::steady_clock::now();
		while(window.IsRunning())
		{
			window.PollEvents();
			const auto now = std::chrono::steady_clock::now();
			const float seconds = std::chrono::duration<float>(now - startTime).count();

			for(const OrbitObject& object : orbitObjects)
			{
				scene.GetTransform(object.entity).worldMatrix = CreateOrbitModel(object, seconds);
			}

			device->BeginFrame();
			renderer.Render(scene, device.get());
			device->Present();
		}

		renderer.Shutdown(device.get());
	}
	catch(const std::exception& e)
	{
		std::cerr << "Exception: " << e.what() << std::endl;
		return -1;
	}

	return 0;
}
