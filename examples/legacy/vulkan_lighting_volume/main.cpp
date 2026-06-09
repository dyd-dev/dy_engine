#include <chrono>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <fstream>
#include <string>
#include <vector>
#include "Core/Types.h"
#include "Graphics/Mesh.h"
#include "Graphics/OBJLoader.h"
#include "Graphics/Shadow.h"
#include "Graphics/ShadowMap.h"
#include "Math/Math.h"
#include "Platform/Window.h"
#include "RHI/IBuffer.h"
#include "RHI/ICommandList.h"
#include "RHI/IDevice.h"
#include "RHI/IPipelineState.h"
#include "RHI/RendererShaderLayout.h"

using namespace dy;

#ifndef DY_SHADER_DIR
#define DY_SHADER_DIR ""
#endif

namespace
{
	namespace Layout = dy::RHI::RendererShaderLayout;

	constexpr float kFloorZ = -0.56f;
	constexpr uint32_t kLightingProfileBinding = Layout::kLightingConstantBinding;
	constexpr uint32_t kShadowMatrixBinding = Layout::kShadowMatrixBinding;

	struct LightingVolumeProfile
	{
		float globalLightDirection[4] = { -0.45f, -0.8f, -0.35f, 1.0f };
		float globalLightColor[4] = { 1.0f, 0.96f, 0.86f, 1.0f };
		float spotLightPosition[4] = { 0.0f, 1.6f, 1.8f, 1.0f };
		float spotLightDirection[4] = { 0.0f, -0.7f, -1.0f, 1.0f };
		float spotLightColor[4] = { 0.55f, 0.72f, 1.0f, 4.0f };
		float volumeParams[4] = { 0.16f, 1.0f, 0.08f, 0.68f };
		float volumeParams2[4] = { 0.88f, 0.0f, 0.0f, 0.0f };
		float cameraPosition[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		float materialParams[4] = { 0.34f, 36.0f, 0.0f, 0.0f };
	};

	std::vector<char> ReadBinaryFile(const std::string& path)
	{
		std::ifstream file(path, std::ios::ate | std::ios::binary);
		if (!file.is_open())
		{
			throw std::runtime_error("Failed to open shader file: " + path);
		}

		const size_t fileSize = static_cast<size_t>(file.tellg());
		std::vector<char> buffer(fileSize);
		file.seekg(0);
		file.read(buffer.data(), fileSize);
		return buffer;
	}

	Math::float3 ComputeAnimatedDirectionalLight(float randomCycle)
	{
		float lightDirX = std::sin(randomCycle * 1.37f + std::cos(randomCycle * 0.61f) * 2.1f);
		float lightDirY = std::cos(randomCycle * 1.11f + std::sin(randomCycle * 0.49f) * 1.6f) * 0.65f;
		float lightDirZ = 0.38f + std::abs(std::sin(randomCycle * 0.83f + 1.7f)) * 0.72f;
		return Math::Normalize(Math::float3(lightDirX, lightDirY, lightDirZ));
	}

	Math::float3 ComputeDirectionalMarkerPosition(const Math::float3& lightDirection)
	{
		return Math::float3(
			lightDirection.x * 0.92f,
			lightDirection.y * 0.92f,
			0.18f + lightDirection.z * 0.92f
		);
	}

	Camera CreateExampleCamera()
	{
		Camera camera = {};
		// Move this position to tune the isometric camera distance and angle.
		camera.worldPosition = Math::float3(3.0f, -3.2f, 2.25f);
		camera.viewMatrix = Math::LookAtRH(
			camera.worldPosition,
			Math::float3(0.0f, 0.0f, 0.12f),
			Math::float3(0.0f, 0.0f, 1.0f)
		);
		camera.projectionMatrix = Math::OrthographicRH_ZO(3.4f, 2.25f, 0.1f, 10.0f);
		return camera;
	}

	Camera CreateSideCamera()
	{
		Camera camera = {};
		camera.worldPosition = Math::float3(4.2f, 0.0f, 1.05f);
		camera.viewMatrix = Math::LookAtRH(
			camera.worldPosition,
			Math::float3(0.0f, 0.0f, 0.08f),
			Math::float3(0.0f, 0.0f, 1.0f)
		);
		camera.projectionMatrix = Math::OrthographicRH_ZO(3.0f, 2.25f, 0.1f, 10.0f);
		return camera;
	}

	Camera CreateTopCamera()
	{
		Camera camera = {};
		camera.worldPosition = Math::float3(0.0f, 0.0f, 4.5f);
		camera.viewMatrix = Math::LookAtRH(
			camera.worldPosition,
			Math::float3(0.0f, 0.0f, 0.0f),
			Math::float3(0.0f, 1.0f, 0.0f)
		);
		camera.projectionMatrix = Math::OrthographicRH_ZO(2.8f, 2.1f, 0.1f, 10.0f);
		return camera;
	}

	Math::float4x4 CreateZUpYawModel(const Math::float3& position, float yawRadians, float uniformScale)
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
		model.m[15] = 1.0f;
		return model;
	}

	bool LoadExampleMesh(Graphics::MeshData& meshData, std::string* outTexturePath)
	{
		const std::string objPath = "examples/vulkan_test/triangle.obj";
		if (Graphics::OBJLoader::Load(objPath, meshData, outTexturePath)) return true;
		if (Graphics::OBJLoader::Load("../" + objPath, meshData, outTexturePath)) return true;
		if (Graphics::OBJLoader::Load("../../" + objPath, meshData, outTexturePath)) return true;
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
		view.viewProj = Math::MultiplyColumnMajor(camera.projectionMatrix, camera.viewMatrix);
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
		std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create(window.GetHandle()));
		if (!device)
		{
			throw std::runtime_error("Failed to initialize RHI device for lighting volume test");
		}

		const std::string shaderDir = DY_SHADER_DIR;
		const std::vector<char> vertexShader = ReadBinaryFile(shaderDir + "/triangle.vert.spv");
		const std::vector<char> pixelShader = ReadBinaryFile(shaderDir + "/triangle.frag.spv");
		const std::vector<char> shadowVertexShader = ReadBinaryFile(shaderDir + "/shadow.vert.spv");
		RHI::GraphicsPipelineDesc pipelineDesc = {};
		pipelineDesc.vertexShader = vertexShader.data();
		pipelineDesc.vertexShaderSize = vertexShader.size();
		pipelineDesc.pixelShader = pixelShader.data();
		pipelineDesc.pixelShaderSize = pixelShader.size();
		pipelineDesc.shadowVertexShader = shadowVertexShader.data();
		pipelineDesc.shadowVertexShaderSize = shadowVertexShader.size();
		pipelineDesc.depthEnable = true;
		pipelineDesc.enableShadowPass = true;
		RHI::IPipelineState* pipeline = device->CreateGraphicsPipeline(pipelineDesc);
		if (!pipeline)
		{
			throw std::runtime_error("Failed to create RHI graphics pipeline for lighting volume test");
		}

		Graphics::MeshData meshData;
		std::string texturePath;
		if (!LoadExampleMesh(meshData, &texturePath))
		{
			throw std::runtime_error("Could not find triangle.obj");
		}
		if (!texturePath.empty())
		{
			std::cout << "Loaded OBJ texture: " << texturePath << std::endl;
		}

		std::vector<float> baseVertices;
		baseVertices.reserve(meshData.vertices.size() * 8); // 사용할 데이터를 미리예약
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
		const dy::RHI::BufferUsage vertexStorageUsage = dy::RHI::BufferUsage::Vertex | dy::RHI::BufferUsage::Storage;
		const dy::RHI::BufferUsage indexStorageUsage = dy::RHI::BufferUsage::Index | dy::RHI::BufferUsage::Storage;
		std::array<RHI::IBuffer*, 2> vertexBuffers = {
			device->CreateBuffer(RHI::BufferDesc{ maxVertexBytes, 8u * static_cast<uint32_t>(sizeof(float)), vertexStorageUsage }),
			device->CreateBuffer(RHI::BufferDesc{ maxVertexBytes, 8u * static_cast<uint32_t>(sizeof(float)), vertexStorageUsage })
		};
		std::array<RHI::IBuffer*, 2> indexBuffers = {
			device->CreateBuffer(RHI::BufferDesc{ maxIndexBytes, static_cast<uint32_t>(sizeof(uint32_t)), indexStorageUsage }),
			device->CreateBuffer(RHI::BufferDesc{ maxIndexBytes, static_cast<uint32_t>(sizeof(uint32_t)), indexStorageUsage })
		};
		std::array<RHI::IBuffer*, 2> lightingBuffers = {
			device->CreateBuffer(RHI::BufferDesc{ static_cast<uint32_t>(sizeof(LightingVolumeProfile)), static_cast<uint32_t>(sizeof(LightingVolumeProfile)), RHI::BufferUsage::Constant }),
			device->CreateBuffer(RHI::BufferDesc{ static_cast<uint32_t>(sizeof(LightingVolumeProfile)), static_cast<uint32_t>(sizeof(LightingVolumeProfile)), RHI::BufferUsage::Constant })
		};
		std::array<RHI::IBuffer*, 2> shadowMatrixBuffers = {
			device->CreateBuffer(RHI::BufferDesc{ static_cast<uint32_t>(sizeof(float) * 16), static_cast<uint32_t>(sizeof(float) * 16), RHI::BufferUsage::Constant }),
			device->CreateBuffer(RHI::BufferDesc{ static_cast<uint32_t>(sizeof(float) * 16), static_cast<uint32_t>(sizeof(float) * 16), RHI::BufferUsage::Constant })
		};

		const float scale = 0.1f;
		const float objectYawRadians = 0.78539816f;
		const Math::float3 objectPosition(0.0f, 0.0f, kFloorZ);
		const Math::float4x4 fixedModel = CreateZUpYawModel(objectPosition, objectYawRadians, scale);

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
			device->BeginFrame();
			const uint32_t frameIndex = device->GetCurrentFrameIndex() % static_cast<uint32_t>(vertexBuffers.size());
			
			const auto now = std::chrono::steady_clock::now();
			const float seconds = std::chrono::duration<float>(now - startTime).count();
			const float randomCycle = seconds * 0.25f;
			const Math::float3 directionalLight = ComputeAnimatedDirectionalLight(randomCycle);
			const float lightDirX = directionalLight.x;
			const float lightDirY = directionalLight.y;
			const float lightDirZ = directionalLight.z;
			const Math::float3 directionalMarkerPosition = ComputeDirectionalMarkerPosition(directionalLight);
			const float daylight = std::max(lightDirZ, 0.0f);
			// directional light 의 정보들을 profile에 갱신
			LightingVolumeProfile profile;
			profile.globalLightDirection[0] = -lightDirX;
			profile.globalLightDirection[1] = -lightDirY;
			profile.globalLightDirection[2] = -lightDirZ;
			profile.globalLightColor[0] = 0.55f;
			profile.globalLightColor[1] = 0.62f;
			profile.globalLightColor[2] = 0.72f;
			profile.globalLightColor[3] = 0.6f;
			//spot light의 정보들을 porfile에 갱신
			const float spotX = 0.65f;
			const float spotY = -0.25f;
			const float spotZ = 1.05f;
			profile.spotLightPosition[0] = spotX;
			profile.spotLightPosition[1] = spotY;
			profile.spotLightPosition[2] = spotZ;
			profile.spotLightDirection[0] = -profile.spotLightPosition[0];
			profile.spotLightDirection[1] = -profile.spotLightPosition[1];
			profile.spotLightDirection[2] = -profile.spotLightPosition[2];
			profile.spotLightColor[0] = 1.0f;
			profile.spotLightColor[1] = 0.78f;
			profile.spotLightColor[2] = 0.38f;
			profile.spotLightColor[3] = 2.0f;
			// 볼륨 효과의 강도, 범위, 그리고 기타 파라미터들을 profile에 갱신
			profile.volumeParams[0] = 0.1f;
			profile.volumeParams[1] = 1.05f + daylight * 0.08f;
			profile.volumeParams[2] = 0.03f;
			profile.volumeParams[3] = 0.96f;
			profile.volumeParams2[0] = 0.90f;
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
			void* lightingData = lightingBuffers[frameIndex]->Map(0);
			if (lightingData == nullptr) throw std::runtime_error("Failed to map lighting constant buffer");
			std::memcpy(lightingData, &profile, sizeof(profile));
			lightingBuffers[frameIndex]->Unmap();

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
			void* shadowMatrixData = shadowMatrixBuffers[frameIndex]->Map(0);
			if (shadowMatrixData == nullptr) throw std::runtime_error("Failed to map shadow matrix constant buffer");
			std::memcpy(shadowMatrixData, lightViewProj.m, sizeof(lightViewProj.m));
			shadowMatrixBuffers[frameIndex]->Unmap();

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
			[[maybe_unused]] const uint32_t shadowIndexCount = static_cast<uint32_t>(frameIndices.size()) - shadowFirstIndex;

			RHI::IBuffer* vertexBuffer = vertexBuffers[frameIndex];
			RHI::IBuffer* indexBuffer = indexBuffers[frameIndex];
			const uint32_t frameVertexBytes = static_cast<uint32_t>(frameVertices.size() * sizeof(float));
			const uint32_t frameIndexBytes = static_cast<uint32_t>(frameIndices.size() * sizeof(uint32_t));
			if (frameVertexBytes > vertexBuffer->GetSize() || frameIndexBytes > indexBuffer->GetSize())
			{
				throw std::runtime_error("Frame mesh exceeded allocated engine buffers");
			}
			void* vertexData = vertexBuffer->Map(0);
			std::memcpy(vertexData, frameVertices.data(), frameVertexBytes);
			vertexBuffer->Unmap();
			void* indexData = indexBuffer->Map(0);
			std::memcpy(indexData, frameIndices.data(), frameIndexBytes);
			indexBuffer->Unmap();

			RHI::ICommandList* cmdList = device->AcquireCommandList();
			cmdList->ClearColor(device->GetBackBuffer(), 0.025f, 0.035f, 0.052f, 1.0f);
			cmdList->BindGraphicsPipeline(pipeline);
			RHI::GeometryBinding geometry = {};
			geometry.vertexBuffer = vertexBuffer;
			geometry.vertexStride = 8u * static_cast<uint32_t>(sizeof(float));
			geometry.vertexOffset = 0;
			geometry.indexBuffer = indexBuffer;
			geometry.indexFormat = RHI::Format::R32_UINT;
			geometry.indexOffset = 0;
			cmdList->BindGeometry(geometry);
			cmdList->BindConstantBuffer(kLightingProfileBinding, lightingBuffers[frameIndex], 0, static_cast<uint32_t>(sizeof(profile)));
			cmdList->BindConstantBuffer(kShadowMatrixBinding, shadowMatrixBuffers[frameIndex], 0, static_cast<uint32_t>(sizeof(lightViewProj.m)));

			struct
			{
				Math::float4x4 viewProj;
				Math::float4x4 model;
				float drawMode;
			} pushData;

			for (const CameraView& cameraView : cameraViews)
			{
				cmdList->SetViewport(cameraView.viewport);
				cmdList->SetScissor(cameraView.scissor);
				pushData.viewProj = cameraView.viewProj;

				Math::float4x4 floorModel = Math::float4x4::Identity();
				pushData.model = floorModel;
				pushData.drawMode = 2.0f;
				cmdList->SetInlineConstants(sizeof(pushData), &pushData);
				cmdList->DrawIndexedInstanced(floorIndexCount, 1, floorFirstIndex, 0, 0);

				pushData.model = fixedModel;
				pushData.drawMode = 1.0f;
				cmdList->SetInlineConstants(sizeof(pushData), &pushData);
				cmdList->DrawIndexedInstanced(objectIndexCount, 1, 0, 0, 0);

				/*
				Math::float4x4 shadowModel = Math::float4x4::Identity();
				pushData.model = shadowModel;
				pushData.drawMode = -2.0f;
				cmdList->SetInlineConstants(sizeof(pushData), &pushData);
				cmdList->DrawIndexedInstanced(shadowIndexCount, 1, shadowFirstIndex, 0, 0);
				*/

				pushData.model = CreateZUpYawModel(directionalMarkerPosition, 0.0f, 0.024f);
				pushData.drawMode = 4.0f;
				cmdList->SetInlineConstants(sizeof(pushData), &pushData);
				cmdList->DrawIndexedInstanced(objectIndexCount, 1, 0, 0, 0);

				const Math::float3 spotMarkerPosition(profile.volumeParams2[1], profile.volumeParams2[2], profile.volumeParams2[3]);
				pushData.model = CreateZUpYawModel(spotMarkerPosition, 0.0f, 0.018f);
				pushData.drawMode = 3.0f;
				cmdList->SetInlineConstants(sizeof(pushData), &pushData);
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
