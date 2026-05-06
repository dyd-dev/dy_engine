#include <chrono>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include "Backends/Vulkan/VulkanDevice.h"
#include "Core/Types.h"
#include "Graphics/Mesh.h"
#include "Graphics/Shadow.h"
#include "Graphics/ShadowMap.h"
#include "Math/Math.h"
#include "Platform/Window.h"
#include "RHI/IBuffer.h"
#include "RHI/ICommandList.h"

using namespace dy;

namespace
{
	constexpr float kFloorZ = -0.56f;

	float Dot(const Math::float3& a, const Math::float3& b)
	{
		return a.x * b.x + a.y * b.y + a.z * b.z;
	}

	Math::float3 Cross(const Math::float3& a, const Math::float3& b)
	{
		return Math::float3(
			a.y * b.z - a.z * b.y,
			a.z * b.x - a.x * b.z,
			a.x * b.y - a.y * b.x
		);
	}

	Math::float3 Normalize(const Math::float3& value)
	{
		const float length = std::sqrt(Dot(value, value));
		if (length <= 0.00001f) return Math::float3(0.0f, 0.0f, 0.0f);
		return Math::float3(value.x / length, value.y / length, value.z / length);
	}

	Math::float4x4 MultiplyColumnMajor(const Math::float4x4& lhs, const Math::float4x4& rhs)
	{
		Math::float4x4 result = {};
		for (int column = 0; column < 4; ++column)
		{
			for (int row = 0; row < 4; ++row)
			{
				float value = 0.0f;
				for (int k = 0; k < 4; ++k)
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
		const Math::float3 forward = Normalize(Math::float3(target.x - eye.x, target.y - eye.y, target.z - eye.z));
		const Math::float3 right = Normalize(Cross(forward, up));
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

	Camera CreateExampleCamera()
	{
		Camera camera = {};
		// Move this position to tune the isometric camera distance and angle.
		camera.worldPosition = Math::float3(3.0f, -3.2f, 2.25f);
		camera.viewMatrix = CreateLookAt(
			camera.worldPosition,
			Math::float3(0.0f, 0.0f, 0.12f),
			Math::float3(0.0f, 0.0f, 1.0f)
		);
		camera.projectionMatrix = CreateOrthographic(3.4f, 2.25f, 0.1f, 10.0f);
		return camera;
	}

	Camera CreateSideCamera()
	{
		Camera camera = {};
		camera.worldPosition = Math::float3(4.2f, 0.0f, 1.05f);
		camera.viewMatrix = CreateLookAt(
			camera.worldPosition,
			Math::float3(0.0f, 0.0f, 0.08f),
			Math::float3(0.0f, 0.0f, 1.0f)
		);
		camera.projectionMatrix = CreateOrthographic(3.0f, 2.25f, 0.1f, 10.0f);
		return camera;
	}

	Camera CreateTopCamera()
	{
		Camera camera = {};
		camera.worldPosition = Math::float3(0.0f, 0.0f, 4.5f);
		camera.viewMatrix = CreateLookAt(
			camera.worldPosition,
			Math::float3(0.0f, 0.0f, 0.0f),
			Math::float3(0.0f, 1.0f, 0.0f)
		);
		camera.projectionMatrix = CreateOrthographic(2.8f, 2.1f, 0.1f, 10.0f);
		return camera;
	}

	Math::float4x4 CreateZUpYawModel(const Math::float3& position, float yawRadians, float uniformScale, float drawMode)
	{
		const float cosYaw = std::cos(yawRadians);
		const float sinYaw = std::sin(yawRadians);

		Math::float4x4 model = Math::float4x4::Identity();
		model.m[0] = uniformScale * cosYaw;
		model.m[1] = uniformScale * sinYaw;
		model.m[4] = uniformScale * -sinYaw;
		model.m[5] = uniformScale * cosYaw;
		model.m[10] = uniformScale;
		model.m[12] = position.x;
		model.m[13] = position.y;
		model.m[14] = position.z;
		model.m[15] = drawMode;
		return model;
	}

	bool LoadExampleMesh(Graphics::MeshData& meshData)
	{
		const std::string objPath = "examples/vulkan_test/triangle.obj";
		if (Graphics::Mesh::LoadFromOBJ(objPath, meshData)) return true;
		if (Graphics::Mesh::LoadFromOBJ("../" + objPath, meshData)) return true;
		if (Graphics::Mesh::LoadFromOBJ("../../" + objPath, meshData)) return true;
		return false;
	}

	void PushVertex(std::vector<float>& vertices, float x, float y, float z, float nx, float ny, float nz, float u, float v)
	{
		vertices.push_back(x);
		vertices.push_back(y);
		vertices.push_back(z);
		vertices.push_back(nx);
		vertices.push_back(ny);
		vertices.push_back(nz);
		vertices.push_back(u);
		vertices.push_back(v);
	}

	void AppendFloorMesh(std::vector<float>& vertices, std::vector<uint32_t>& indices)
	{
		const uint32_t start = static_cast<uint32_t>(vertices.size() / 8);
		PushVertex(vertices, -1.15f, -1.02f, kFloorZ, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
		PushVertex(vertices, 1.15f, -1.02f, kFloorZ, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f);
		PushVertex(vertices, 1.15f, 0.88f, kFloorZ, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);
		PushVertex(vertices, -1.15f, 0.88f, kFloorZ, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f);

		indices.push_back(start + 0);
		indices.push_back(start + 2);
		indices.push_back(start + 1);
		indices.push_back(start + 0);
		indices.push_back(start + 3);
		indices.push_back(start + 2);
	}

	void AppendMeshData(const Graphics::MeshData& meshData, std::vector<float>& vertices, std::vector<uint32_t>& indices)
	{
		const uint32_t start = static_cast<uint32_t>(vertices.size() / 8);
		for (const Graphics::Vertex& vertex : meshData.vertices)
		{
			PushVertex(
				vertices,
				vertex.position.x,
				vertex.position.y,
				vertex.position.z,
				vertex.normal.x,
				vertex.normal.y,
				vertex.normal.z,
				vertex.uv.x,
				vertex.uv.y
			);
		}

		for (uint32_t index : meshData.indices)
		{
			indices.push_back(start + index);
		}
	}

	struct CameraView
	{
		Camera camera = {};
		Math::float4x4 viewProj = Math::float4x4::Identity();
		RHI::Viewport viewport = {};
		RHI::Rect scissor = {};
	};

	CameraView CreateCameraView(const Camera& camera, const RHI::Viewport& viewport, const RHI::Rect& scissor)
	{
		CameraView view = {};
		view.camera = camera;
		view.viewProj = MultiplyColumnMajor(camera.projectionMatrix, camera.viewMatrix);
		view.viewport = viewport;
		view.scissor = scissor;
		return view;
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

		std::vector<float> baseVertices;
		baseVertices.reserve(meshData.vertices.size() * 8);
		for (const auto& v : meshData.vertices)
		{
			baseVertices.push_back(v.position.x);
			baseVertices.push_back(v.position.y);
			baseVertices.push_back(v.position.z);
			baseVertices.push_back(v.normal.x);
			baseVertices.push_back(v.normal.y);
			baseVertices.push_back(v.normal.z);
			baseVertices.push_back(v.uv.x);
			baseVertices.push_back(v.uv.y);
		}

		std::vector<uint32_t> baseIndices = meshData.indices;
		const uint32_t objectIndexCount = static_cast<uint32_t>(baseIndices.size());
		const uint32_t floorFirstIndex = objectIndexCount;
		AppendFloorMesh(baseVertices, baseIndices);
		const uint32_t floorIndexCount = static_cast<uint32_t>(baseIndices.size()) - floorFirstIndex;
		const uint32_t maxShadowVertexCount = static_cast<uint32_t>(meshData.vertices.size());
		const uint32_t maxShadowIndexCount = maxShadowVertexCount > 2 ? (maxShadowVertexCount - 2) * 3 : 0;
		const uint32_t maxVertexBytes = static_cast<uint32_t>(baseVertices.size() * sizeof(float) + maxShadowVertexCount * 8u * sizeof(float));
		const uint32_t maxIndexBytes = static_cast<uint32_t>((baseIndices.size() + maxShadowIndexCount) * sizeof(uint32_t));
		std::array<RHI::IBuffer*, 2> vertexBuffers = {
			device->CreateBuffer(RHI::BufferDesc{ maxVertexBytes, 8u * static_cast<uint32_t>(sizeof(float)), RHI::BufferUsage::Vertex }),
			device->CreateBuffer(RHI::BufferDesc{ maxVertexBytes, 8u * static_cast<uint32_t>(sizeof(float)), RHI::BufferUsage::Vertex })
		};
		std::array<RHI::IBuffer*, 2> indexBuffers = {
			device->CreateBuffer(RHI::BufferDesc{ maxIndexBytes, static_cast<uint32_t>(sizeof(uint32_t)), RHI::BufferUsage::Index }),
			device->CreateBuffer(RHI::BufferDesc{ maxIndexBytes, static_cast<uint32_t>(sizeof(uint32_t)), RHI::BufferUsage::Index })
		};

		const float scale = 0.1f;
		const float objectYawRadians = 0.78539816f;
		const Math::float3 objectPosition(0.0f, 0.0f, kFloorZ);
		const Math::float4x4 fixedModel = CreateZUpYawModel(objectPosition, objectYawRadians, scale, 1.0f);

		const Camera mainCamera = CreateExampleCamera();
		const std::array<CameraView, 3> cameraViews = {
			CreateCameraView(mainCamera, RHI::Viewport{ 0.0f, 0.0f, 896.0f, 720.0f, 0.0f, 1.0f }, RHI::Rect{ 0, 0, 896, 720 }),
			CreateCameraView(CreateSideCamera(), RHI::Viewport{ 896.0f, 0.0f, 384.0f, 360.0f, 0.0f, 1.0f }, RHI::Rect{ 896, 0, 384, 360 }),
			CreateCameraView(CreateTopCamera(), RHI::Viewport{ 896.0f, 360.0f, 384.0f, 360.0f, 0.0f, 1.0f }, RHI::Rect{ 896, 360, 384, 360 })
		};
		const auto startTime = std::chrono::steady_clock::now();

		while (window.IsRunning())
		{
			window.PollEvents();

			const auto now = std::chrono::steady_clock::now();
			const float seconds = std::chrono::duration<float>(now - startTime).count();
			const float randomCycle = seconds * 0.25f;
			float lightDirX = std::sin(randomCycle * 1.37f + std::cos(randomCycle * 0.61f) * 2.1f);
			float lightDirY = std::cos(randomCycle * 1.11f + std::sin(randomCycle * 0.49f) * 1.6f) * 0.65f;
			float lightDirZ = 0.38f + std::abs(std::sin(randomCycle * 0.83f + 1.7f)) * 0.72f;
			const float lightDirLength = std::sqrt(lightDirX * lightDirX + lightDirY * lightDirY + lightDirZ * lightDirZ);
			lightDirX /= lightDirLength;
			lightDirY /= lightDirLength;
			lightDirZ /= lightDirLength;
			const float daylight = std::max(lightDirZ, 0.0f);

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
			profile.cameraPosition[0] = mainCamera.worldPosition.x;
			profile.cameraPosition[1] = mainCamera.worldPosition.y;
			profile.cameraPosition[2] = mainCamera.worldPosition.z;
			profile.cameraPosition[3] = 1.0f;
			profile.materialParams[0] = 0.42f;
			profile.materialParams[1] = 42.0f;
			profile.materialParams[2] = kFloorZ;
			profile.materialParams[3] = 0.003f;
			device->SetLightingVolumeProfile(profile);

			// Shadow Map용 Light-Space ViewProjection 갱신.
			// Directional Light가 매 프레임 흔들리므로 LightViewProj도 매 프레임 재계산.
			Graphics::ShadowMapDesc shadowMapDesc = {};
			shadowMapDesc.resolution    = 2048;
			shadowMapDesc.orthoWidth    = 4.5f;
			shadowMapDesc.orthoHeight   = 4.5f;
			shadowMapDesc.nearPlane     = 0.1f;
			shadowMapDesc.farPlane      = 18.0f;
			shadowMapDesc.sceneCenter   = Math::float3(0.0f, 0.0f, kFloorZ);
			shadowMapDesc.lightDistance = 6.0f;
			const Math::float4x4 lightViewProj = Graphics::ComputeDirectionalLightViewProj(
				Math::float3(lightDirX, lightDirY, lightDirZ),
				shadowMapDesc);
			device->SetShadowLightMatrix(lightViewProj.m);

			std::vector<float> frameVertices = baseVertices;
			std::vector<uint32_t> frameIndices = baseIndices;
			const uint32_t shadowFirstIndex = static_cast<uint32_t>(frameIndices.size());
			const Graphics::ShadowDesc shadowDesc = {
				Math::float3(lightDirX, lightDirY, lightDirZ),
				kFloorZ,
				0.003f
			};
			const Graphics::MeshData shadowMesh = Graphics::BuildShadowMesh(
				meshData,
				fixedModel,
				shadowDesc
			);
			AppendMeshData(shadowMesh, frameVertices, frameIndices);
			const uint32_t shadowIndexCount = static_cast<uint32_t>(frameIndices.size()) - shadowFirstIndex;

			device->BeginFrame();
			const uint32_t frameIndex = device->GetCurrentFrameIndex() % static_cast<uint32_t>(vertexBuffers.size());
			RHI::IBuffer* vertexBuffer = vertexBuffers[frameIndex];
			RHI::IBuffer* indexBuffer = indexBuffers[frameIndex];
			const uint32_t frameVertexBytes = static_cast<uint32_t>(frameVertices.size() * sizeof(float));
			const uint32_t frameIndexBytes = static_cast<uint32_t>(frameIndices.size() * sizeof(uint32_t));
			if (frameVertexBytes > vertexBuffer->GetSize() || frameIndexBytes > indexBuffer->GetSize())
			{
				throw std::runtime_error("Frame mesh exceeded allocated engine buffers");
			}
			void* vertexData = vertexBuffer->Map(0, frameVertexBytes);
			std::memcpy(vertexData, frameVertices.data(), frameVertexBytes);
			vertexBuffer->Unmap();
			void* indexData = indexBuffer->Map(0, frameIndexBytes);
			std::memcpy(indexData, frameIndices.data(), frameIndexBytes);
			indexBuffer->Unmap();

			RHI::ICommandList* cmdList = device->AcquireCommandList();
			cmdList->ClearColor(device->GetBackBuffer(), 0.025f, 0.035f, 0.052f, 1.0f);
			cmdList->BindVertexBuffer(vertexBuffer, 8u * static_cast<uint32_t>(sizeof(float)), 0);
			cmdList->BindIndexBuffer(indexBuffer, RHI::Format::R32_UINT, 0);

			struct
			{
				Math::float4x4 viewProj;
				Math::float4x4 model;
			} pushData;

			for (const CameraView& cameraView : cameraViews)
			{
				cmdList->SetViewport(cameraView.viewport);
				cmdList->SetScissor(cameraView.scissor);
				pushData.viewProj = cameraView.viewProj;

				Math::float4x4 floorModel = Math::float4x4::Identity();
				floorModel.m[15] = 2.0f;
				pushData.model = floorModel;
				cmdList->SetPushConstants(sizeof(pushData), &pushData);
				cmdList->DrawIndexedInstanced(floorIndexCount, 1, floorFirstIndex, 0, 0);

				pushData.model = fixedModel;
				cmdList->SetPushConstants(sizeof(pushData), &pushData);
				cmdList->DrawIndexedInstanced(objectIndexCount, 1, 0, 0, 0);

				Math::float4x4 shadowModel = Math::float4x4::Identity();
				shadowModel.m[15] = -2.0f;
				pushData.model = shadowModel;
				cmdList->SetPushConstants(sizeof(pushData), &pushData);
				cmdList->DrawIndexedInstanced(shadowIndexCount, 1, shadowFirstIndex, 0, 0);

				Math::float4x4 sunMarkerModel = Math::float4x4::Identity();
				sunMarkerModel.m[0] = 0.018f;
				sunMarkerModel.m[5] = 0.018f;
				sunMarkerModel.m[10] = 0.018f;
				sunMarkerModel.m[12] = profile.volumeParams2[1];
				sunMarkerModel.m[13] = profile.volumeParams2[2];
				sunMarkerModel.m[14] = profile.volumeParams2[3];
				sunMarkerModel.m[15] = 1.0f;

				pushData.model = sunMarkerModel;
				cmdList->SetPushConstants(sizeof(pushData), &pushData);
				cmdList->DrawIndexedInstanced(objectIndexCount, 1, 0, 0, 0);
			}
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
