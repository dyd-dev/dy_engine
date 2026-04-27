#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include "Backends/Vulkan/VulkanDevice.h"
#include "Graphics/Mesh.h"
#include "Math/Math.h"
#include "Platform/Window.h"
#include "RHI/ICommandList.h"

using namespace dy;

namespace
{
	bool LoadExampleMesh(Graphics::MeshData& meshData)
	{
		const std::string objPath = "examples/vulkan_test/triangle.obj";
		if (Graphics::Mesh::LoadFromOBJ(objPath, meshData)) return true;
		if (Graphics::Mesh::LoadFromOBJ("../" + objPath, meshData)) return true;
		if (Graphics::Mesh::LoadFromOBJ("../../" + objPath, meshData)) return true;
		return false;
	}
}

int main()
{
	try
	{
		Platform::Window window(1280, 720, "Vulkan Lighting Volume Test");
		std::unique_ptr<VulkanDevice> device = std::make_unique<VulkanDevice>();

		if (device->InitializeForTest(window.GetHandle(), VULKAN_LIGHTING_VOLUME_SHADER_DIR) != 0)
		{
			throw std::runtime_error("Failed to initialize VulkanDevice for lighting volume test");
		}

		Graphics::MeshData meshData;
		if (!LoadExampleMesh(meshData))
		{
			throw std::runtime_error("Could not find triangle.obj");
		}

		std::vector<float> flatVertices;
		flatVertices.reserve(meshData.vertices.size() * 8);
		for (const auto& v : meshData.vertices)
		{
			flatVertices.push_back(v.position.x);
			flatVertices.push_back(v.position.y);
			flatVertices.push_back(v.position.z);
			flatVertices.push_back(v.normal.x);
			flatVertices.push_back(v.normal.y);
			flatVertices.push_back(v.normal.z);
			flatVertices.push_back(v.uv.x);
			flatVertices.push_back(v.uv.y);
		}

		device->UploadTestMesh(flatVertices, meshData.indices);

		const float scale = 0.1f;
		const float yRadians = 0.78539816f;
		const float xRadians = -0.61547971f;
		const float cosY = std::cos(yRadians);
		const float sinY = std::sin(yRadians);
		const float cosX = std::cos(xRadians);
		const float sinX = std::sin(xRadians);

		Math::float4x4 fixedModel = Math::float4x4::Identity();
		fixedModel.m[0] = scale * cosY;
		fixedModel.m[1] = scale * sinX * sinY;
		fixedModel.m[2] = scale * cosX * sinY;
		fixedModel.m[4] = 0.0f;
		fixedModel.m[5] = scale * cosX;
		fixedModel.m[6] = scale * -sinX;
		fixedModel.m[8] = scale * -sinY;
		fixedModel.m[9] = scale * sinX * cosY;
		fixedModel.m[10] = scale * cosX * cosY;

		Math::float4x4 fixedViewProj = Math::float4x4::Identity();
		fixedViewProj.m[0] = 0.85f;
		fixedViewProj.m[5] = 0.85f;
		fixedViewProj.m[10] = 0.5f;
		fixedViewProj.m[14] = 0.5f;
		const auto startTime = std::chrono::steady_clock::now();

		while (window.IsRunning())
		{
			window.PollEvents();

			const auto now = std::chrono::steady_clock::now();
			const float seconds = std::chrono::duration<float>(now - startTime).count();
			const float randomCycle = seconds * 0.25f;
			float lightDirX = std::sin(randomCycle * 1.37f + std::cos(randomCycle * 0.61f) * 2.1f);
			float lightDirY = 0.18f + std::abs(std::sin(randomCycle * 0.83f + 1.7f)) * 0.82f;
			float lightDirZ = std::cos(randomCycle * 1.11f + std::sin(randomCycle * 0.49f) * 1.6f) * 0.65f;
			const float lightDirLength = std::sqrt(lightDirX * lightDirX + lightDirY * lightDirY + lightDirZ * lightDirZ);
			lightDirX /= lightDirLength;
			lightDirY /= lightDirLength;
			lightDirZ /= lightDirLength;
			const float daylight = std::max(lightDirY, 0.0f);

			VulkanLightingVolumeProfile profile;
			profile.globalLightDirection[0] = -lightDirX;
			profile.globalLightDirection[1] = -lightDirY;
			profile.globalLightDirection[2] = -lightDirZ;
			profile.globalLightColor[0] = 0.55f;
			profile.globalLightColor[1] = 0.62f;
			profile.globalLightColor[2] = 0.72f;
			profile.globalLightColor[3] = 0.38f;

			const float spotX = lightDirX * 0.62f;
			const float spotY = lightDirY * 0.45f - 0.05f;
			const float spotZ = 0.85f + lightDirZ * 0.18f;
			profile.spotLightPosition[0] = spotX;
			profile.spotLightPosition[1] = spotY;
			profile.spotLightPosition[2] = spotZ;
			profile.spotLightDirection[0] = -profile.spotLightPosition[0];
			profile.spotLightDirection[1] = -profile.spotLightPosition[1];
			profile.spotLightDirection[2] = -profile.spotLightPosition[2];
			profile.spotLightColor[0] = 1.0f;
			profile.spotLightColor[1] = 0.78f;
			profile.spotLightColor[2] = 0.38f;
			profile.spotLightColor[3] = 8.0f;
			profile.volumeParams[0] = 0.1f;
			profile.volumeParams[1] = 1.05f + daylight * 0.08f;
			profile.volumeParams[2] = 0.03f;
			profile.volumeParams[3] = 0.90f;
			profile.volumeParams2[0] = 0.55f;
			profile.volumeParams2[1] = spotX;
			profile.volumeParams2[2] = spotY;
			profile.volumeParams2[3] = spotZ;
			device->SetLightingVolumeProfile(profile);

			device->BeginFrame();
			RHI::ICommandList* cmdList = device->AcquireCommandList();
			cmdList->ClearColor(device->GetBackBuffer(), 0.025f, 0.035f, 0.052f, 1.0f);

			struct
			{
				Math::float4x4 viewProj;
				Math::float4x4 model;
			} pushData;

			pushData.viewProj = fixedViewProj;
			pushData.model = fixedModel;

			cmdList->SetPushConstants(sizeof(pushData), &pushData);
			cmdList->DrawInstanced(0, 1, 0, 0);

			Math::float4x4 sunMarkerModel = Math::float4x4::Identity();
			sunMarkerModel.m[0] = 0.018f;
			sunMarkerModel.m[5] = 0.018f;
			sunMarkerModel.m[10] = 0.018f;
			sunMarkerModel.m[12] = profile.volumeParams2[1];
			sunMarkerModel.m[13] = profile.volumeParams2[2];
			sunMarkerModel.m[14] = 0.0f;

			pushData.model = sunMarkerModel;
			cmdList->SetPushConstants(sizeof(pushData), &pushData);
			cmdList->DrawInstanced(0, 1, 0, 0);
			cmdList->Close();

			RHI::ICommandList* submitLists[] = { cmdList };
			device->Submit(submitLists, 1);
			device->Present();
		}
	}
	catch (const std::exception& e)
	{
		std::cerr << "Exception: " << e.what() << std::endl;
		return -1;
	}

	return 0;
}
