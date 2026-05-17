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
	constexpr uint32_t kObjectCount = 100;
	constexpr float kGoldenAngle = 2.39996323f;

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

	float Dot(const Math::float3& a, const Math::float3& b)
	{
		return a.x * b.x + a.y * b.y + a.z * b.z;
	}

	Math::float3 Cross(const Math::float3& a, const Math::float3& b)
	{
		return Math::float3(
			a.y * b.z - a.z * b.y,
			a.z * b.x - a.x * b.z,
			a.x * b.y - a.y * b.x);
	}

	Math::float3 Subtract(const Math::float3& a, const Math::float3& b)
	{
		return Math::float3(a.x - b.x, a.y - b.y, a.z - b.z);
	}

	Math::float3 NormalizeOr(const Math::float3& value, const Math::float3& fallback)
	{
		const float lengthSquared = Dot(value, value);
		if(lengthSquared <= 1.0e-8f) return fallback;
		const float invLength = 1.0f / std::sqrt(lengthSquared);
		return Math::float3(value.x * invLength, value.y * invLength, value.z * invLength);
	}

	Math::float4x4 MultiplyColumnMajor(const Math::float4x4& lhs, const Math::float4x4& rhs)
	{
		Math::float4x4 result = {};
		for(int column = 0; column < 4; ++column)
		{
			for(int row = 0; row < 4; ++row)
			{
				float value = 0.0f;
				for(int k = 0; k < 4; ++k)
				{
					value += lhs.m[k * 4 + row] * rhs.m[column * 4 + k];
				}
				result.m[column * 4 + row] = value;
			}
		}
		return result;
	}

	Math::float4x4 CreateLookAt(const Math::float3& eye, const Math::float3& target, const Math::float3& up)
	{
		const Math::float3 forward = NormalizeOr(Subtract(target, eye), Math::float3(0.0f, 1.0f, 0.0f));
		const Math::float3 right = NormalizeOr(Cross(forward, up), Math::float3(1.0f, 0.0f, 0.0f));
		const Math::float3 cameraUp = Cross(right, forward);

		Math::float4x4 view = Math::float4x4::Identity();
		view.m[0] = right.x;
		view.m[4] = right.y;
		view.m[8] = right.z;
		view.m[12] = -Dot(right, eye);
		view.m[1] = cameraUp.x;
		view.m[5] = cameraUp.y;
		view.m[9] = cameraUp.z;
		view.m[13] = -Dot(cameraUp, eye);
		view.m[2] = -forward.x;
		view.m[6] = -forward.y;
		view.m[10] = -forward.z;
		view.m[14] = Dot(forward, eye);
		return view;
	}

	Math::float4x4 CreateOrthographic(float width, float height, float nearPlane, float farPlane)
	{
		Math::float4x4 projection = Math::float4x4::Identity();
		projection.m[0] = 2.0f / width;
		projection.m[5] = -2.0f / height;
		projection.m[10] = 1.0f / (nearPlane - farPlane);
		projection.m[14] = nearPlane / (nearPlane - farPlane);
		return projection;
	}

	Math::float4x4 CreateViewProjection()
	{
		const Math::float3 eye(5.6f, -6.4f, 4.8f);
		const Math::float3 target(0.0f, 0.0f, 0.0f);
		const Math::float4x4 view = CreateLookAt(eye, target, Math::float3(0.0f, 0.0f, 1.0f));
		const Math::float4x4 projection = CreateOrthographic(9.4f, 6.0f, 0.1f, 18.0f);
		return MultiplyColumnMajor(projection, view);
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
		const float s = 0.16f;
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

		std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create(window.GetHandle()));
		if(!device)
		{
			throw std::runtime_error("Failed to initialize RHI device for orbit lit scene");
		}

		Graphics::Scene scene;
		Graphics::AssetManager assets(scene);

		const Math::float3 lightPosition(0.0f, 0.0f, 0.0f);
		[[maybe_unused]] const uint32_t centerLight = scene.CreatePointLight(
			lightPosition,
			8.8f,
			Math::float3(1.0f, 0.88f, 0.62f),
			9.4f,
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
			const uint32_t ring = index % 5u;
			const float heightT = static_cast<float>((index * 7u) % 31u) / 30.0f;
			object.radius = 0.85f + static_cast<float>(ring) * 0.62f + static_cast<float>((index * 17u) % 29u) * 0.012f;
			object.angle = static_cast<float>(index) * kGoldenAngle;
			object.speed = 0.16f + static_cast<float>((index * 11u) % 23u) * 0.006f;
			object.height = -1.45f + heightT * 2.9f;
			object.scale = 0.045f + static_cast<float>((index * 5u) % 13u) * 0.0025f;
			object.spin = 0.25f + static_cast<float>((index * 3u) % 19u) * 0.018f;

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
		rendererConfig.cameraPosition = Math::float3(5.6f, -6.4f, 4.8f);
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
		rendererConfig.shadowMap.farPlane = 10.5f;
		rendererConfig.shadowMap.spotFovYRadians = 2.35f;
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
