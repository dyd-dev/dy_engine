#include "Backends/Vulkan/VulkanDevice.h"
#include "Graphics/OBJLoader.h"
#include "Math/Math.h"
#include "Platform/Window.h"
#include "RHI/ICommandList.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

using namespace dy;

namespace
{
	struct PushConstants
	{
		Math::float4x4 worldViewProjection;
		Math::float4x4 world;
	};

	Math::float4x4 RotationY(float radians)
	{
		Math::float4x4 result = Math::float4x4::Identity();
		const float c = std::cos(radians);
		const float s = std::sin(radians);
		result.m[0] = c;
		result.m[2] = -s;
		result.m[8] = s;
		result.m[10] = c;
		return result;
	}

	Math::float4x4 UniformScale(float scale)
	{
		Math::float4x4 result = Math::float4x4::Identity();
		result.m[0] = scale;
		result.m[5] = scale;
		result.m[10] = scale;
		return result;
	}

	std::vector<float> BuildInterleavedVertices(const Graphics::MeshData& mesh)
	{
		std::vector<float> vertices;
		vertices.reserve(mesh.vertices.size() * 8);
		for(const Graphics::Vertex& vertex : mesh.vertices)
		{
			vertices.push_back(vertex.position.x);
			vertices.push_back(vertex.position.y);
			vertices.push_back(vertex.position.z);
			vertices.push_back(vertex.normal.x);
			vertices.push_back(vertex.normal.y);
			vertices.push_back(vertex.normal.z);
			vertices.push_back(vertex.uv.x);
			vertices.push_back(vertex.uv.y);
		}
		return vertices;
	}

	Graphics::MeshData BuildFallbackMesh()
	{
		Graphics::MeshData mesh;
		mesh.vertices = {
			{ Math::float3(-0.8f, -0.5f, 0.0f), Math::float3(0.0f, 0.0f, 1.0f), Math::float2(0.0f, 1.0f) },
			{ Math::float3(0.8f, -0.5f, 0.0f), Math::float3(0.0f, 0.0f, 1.0f), Math::float2(1.0f, 1.0f) },
			{ Math::float3(0.0f, 0.75f, 0.0f), Math::float3(0.0f, 0.0f, 1.0f), Math::float2(0.5f, 0.0f) },
		};
		mesh.indices = { 0, 1, 2 };
		return mesh;
	}
}

int main()
{
	Platform::Window window(1280, 720, "Vulkan_Test");

	Graphics::MeshData mesh;
	if(!Graphics::OBJLoader::Load("examples/vulkan_test/triangle.obj", mesh) || mesh.vertices.empty() || mesh.indices.empty())
	{
		std::cerr << "Failed to load Vulkan example OBJ. Using fallback triangle.\n";
		mesh = BuildFallbackMesh();
	}

	std::vector<float> vertices = BuildInterleavedVertices(mesh);
	std::vector<uint32_t> indices = mesh.indices;

	VulkanDevice device;
	if(device.InitializeForTest(window.GetHandle(), DY_VULKAN_EXAMPLE_SHADER_DIR) != 0)
	{
		std::cerr << "Failed to initialize Vulkan example device.\n";
		return -1;
	}

	if(!device.UploadTestMesh(vertices, indices))
	{
		std::cerr << "Failed to upload Vulkan example mesh.\n";
		return -1;
	}

	const Math::float4x4 view = Math::LookAtRH(
		Math::float3(0.0f, 1.8f, 4.5f),
		Math::float3(0.0f, 0.0f, 0.0f),
		Math::float3(0.0f, 1.0f, 0.0f));
	const Math::float4x4 projection = Math::OrthographicRH_ZO(3.2f, 1.8f, 0.1f, 20.0f);
	const Math::float4x4 viewProjection = Math::MultiplyColumnMajor(projection, view);

	const auto startTime = std::chrono::steady_clock::now();

	while(window.IsRunning())
	{
		window.PollEvents();

		const auto now = std::chrono::steady_clock::now();
		const float seconds = std::chrono::duration<float>(now - startTime).count();
		const Math::float4x4 world = Math::MultiplyColumnMajor(RotationY(seconds * 0.65f), UniformScale(0.14f));
		const PushConstants pushConstants = {
			Math::MultiplyColumnMajor(viewProjection, world),
			world
		};

		device.BeginFrame();
		RHI::ICommandList* cmdList = device.AcquireCommandList();
		cmdList->ClearColor(nullptr, 0.04f, 0.05f, 0.07f, 1.0f);
		cmdList->SetPushConstants(static_cast<uint32_t>(sizeof(pushConstants)), &pushConstants);
		cmdList->DrawIndexedInstanced(static_cast<uint32_t>(indices.size()), 1, 0, 0, 0);
		cmdList->Close();

		device.Submit(&cmdList, 1);
		device.Present();
	}

	return 0;
}
