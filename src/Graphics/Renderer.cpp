#include "Graphics/Renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "Graphics/Scene.h"
#include "RHI/IBuffer.h"
#include "RHI/ICommandList.h"
#include "RHI/IDevice.h"
#include "RHI/IPipelineState.h"
#include "RHI/ITexture.h"

using namespace dy;
using namespace dy::Graphics;

namespace Layout = dy::RHI::RendererShaderLayout;

namespace
{
	constexpr uint32_t kMaterialBaseColorTextureSlot = 0;
	constexpr uint32_t kMaterialMetallicRoughnessTextureSlot = 1;
	constexpr uint32_t kMaterialNormalTextureSlot = 2;
	constexpr uint32_t kMaterialOcclusionTextureSlot = 3;
	constexpr uint32_t kMaterialEmissiveTextureSlot = 4;

	struct ShadowBounds
	{
		Math::float3 min = Math::float3(0.0f, 0.0f, 0.0f);
		Math::float3 max = Math::float3(0.0f, 0.0f, 0.0f);
		bool valid = false;
	};

	struct RendererVertex
	{
		float px = 0.0f;
		float py = 0.0f;
		float pz = 0.0f;
		float nx = 0.0f;
		float ny = 0.0f;
		float nz = 1.0f;
		float u = 0.0f;
		float v = 0.0f;
		float tx = 1.0f;
		float ty = 0.0f;
		float tz = 0.0f;
		float tw = 1.0f;
	};

	[[nodiscard]] std::vector<char> ReadBinaryFile(const char* filepath)
	{
		std::ifstream file(filepath, std::ios::binary);
		if(!file.is_open())
		{
			throw std::runtime_error(std::string("Failed to open shader file: ") + filepath);
		}

		file.seekg(0, std::ios::end);
		const std::streamoff size = file.tellg();
		file.seekg(0, std::ios::beg);

		std::vector<char> content(static_cast<size_t>(size));
		if(size > 0)
		{
			file.read(content.data(), size);
		}

		return content;
	}

	[[nodiscard]] bool ResolveRendererShaderPaths(
		const RendererDesc& config,
		const RHI::RendererShaderPaths& defaultShaderPaths,
		const char*& vertexShaderPath,
		const char*& pixelShaderPath,
		const char*& shadowVertexShaderPath)
	{
		vertexShaderPath = config.vertexShaderPath != nullptr ? config.vertexShaderPath : defaultShaderPaths.vertexShaderPath;
		pixelShaderPath = config.pixelShaderPath != nullptr ? config.pixelShaderPath : defaultShaderPaths.pixelShaderPath;
		shadowVertexShaderPath = config.shadowVertexShaderPath != nullptr
			? config.shadowVertexShaderPath
			: defaultShaderPaths.shadowVertexShaderPath;

		return vertexShaderPath != nullptr && pixelShaderPath != nullptr && (!config.enableShadows || shadowVertexShaderPath != nullptr);
	}

	[[nodiscard]] std::vector<RendererVertex> BuildRendererVertices(const dy::Mesh& mesh)
	{
		std::vector<RendererVertex> vertices;
		vertices.reserve(mesh.vertices.size());
		for(const Vertex& vertex : mesh.vertices)
		{
			RendererVertex rendererVertex = {};
			rendererVertex.px = vertex.px;
			rendererVertex.py = vertex.py;
			rendererVertex.pz = vertex.pz;
			rendererVertex.nx = vertex.nx;
			rendererVertex.ny = vertex.ny;
			rendererVertex.nz = vertex.nz;
			rendererVertex.u = vertex.u;
			rendererVertex.v = vertex.v;
			rendererVertex.tx = vertex.tx;
			rendererVertex.ty = vertex.ty;
			rendererVertex.tz = vertex.tz;
			rendererVertex.tw = vertex.tw;
			vertices.push_back(rendererVertex);
		}
		return vertices;
	}

	[[nodiscard]] std::vector<uint32_t> BuildRendererIndices(const dy::Mesh& mesh)
	{
		if(!mesh.indices.empty())
		{
			return mesh.indices;
		}

		std::vector<uint32_t> indices;
		indices.reserve(mesh.vertices.size());
		for(uint32_t vertexIndex = 0; vertexIndex < static_cast<uint32_t>(mesh.vertices.size()); ++vertexIndex)
		{
			indices.push_back(vertexIndex);
		}
		return indices;
	}

	[[nodiscard]] const DirectionalLight* GetPrimaryDirectionalLight(const Scene& scene)
	{
		return scene.GetDirectionalLightCount() > 0 ? &scene.GetDirectionalLight(0) : nullptr;
	}

	[[nodiscard]] const PointLight* GetPrimaryPointLight(const Scene& scene)
	{
		return scene.GetPointLightCount() > 0 ? &scene.GetPointLight(0) : nullptr;
	}

	[[nodiscard]] float Dot(const Math::float3& a, const Math::float3& b)
	{
		return a.x * b.x + a.y * b.y + a.z * b.z;
	}

	[[nodiscard]] Math::float3 Subtract(const Math::float3& a, const Math::float3& b)
	{
		return Math::float3(a.x - b.x, a.y - b.y, a.z - b.z);
	}

	[[nodiscard]] Math::float3 NormalizeOr(const Math::float3& value, const Math::float3& fallback)
	{
		const float lengthSquared = Dot(value, value);
		if(lengthSquared <= 1.0e-8f) return fallback;
		const float invLength = 1.0f / std::sqrt(lengthSquared);
		return Math::float3(value.x * invLength, value.y * invLength, value.z * invLength);
	}

	[[nodiscard]] Math::float3 TransformPoint(const Math::float4x4& matrix, const Vertex& vertex)
	{
		return Math::float3(
			matrix.m[0] * vertex.px + matrix.m[4] * vertex.py + matrix.m[8] * vertex.pz + matrix.m[12],
			matrix.m[1] * vertex.px + matrix.m[5] * vertex.py + matrix.m[9] * vertex.pz + matrix.m[13],
			matrix.m[2] * vertex.px + matrix.m[6] * vertex.py + matrix.m[10] * vertex.pz + matrix.m[14]);
	}

	void IncludePoint(ShadowBounds& bounds, const Math::float3& point)
	{
		if(!bounds.valid)
		{
			bounds.min = point;
			bounds.max = point;
			bounds.valid = true;
			return;
		}

		bounds.min.x = std::min(bounds.min.x, point.x);
		bounds.min.y = std::min(bounds.min.y, point.y);
		bounds.min.z = std::min(bounds.min.z, point.z);
		bounds.max.x = std::max(bounds.max.x, point.x);
		bounds.max.y = std::max(bounds.max.y, point.y);
		bounds.max.z = std::max(bounds.max.z, point.z);
	}

	[[nodiscard]] ShadowBounds ComputeShadowBounds(const Scene& scene)
	{
		ShadowBounds bounds = {};
		const uint32_t entityCount = scene.GetEntityCount();
		for(uint32_t entityIndex = 0; entityIndex < entityCount; ++entityIndex)
		{
			const EntityID entity = static_cast<EntityID>(entityIndex);
			const RenderFlags& flags = scene.GetRenderFlags(entity);
			if(!flags.castShadow && !flags.receiveShadow) continue;

			const MeshID meshId = scene.GetEntityMesh(entity);
			if(!IsValid(meshId)) continue;

			const dy::Mesh& mesh = scene.GetMesh(meshId);
			const Transform& transform = scene.GetTransform(entity);
			for(const Vertex& vertex : mesh.vertices)
			{
				IncludePoint(bounds, TransformPoint(transform.worldMatrix, vertex));
			}
		}
		return bounds;
	}
}

bool Renderer::Initialize(RHI::IDevice* device, const RendererDesc& config)
{
	static_assert(Layout::kPushConstantRangeSize == sizeof(Layout::DrawConstants), "Renderer draw constants size mismatch.");
	static_assert(sizeof(RendererVertex) == sizeof(float) * Layout::kRendererVertexFloatCount, "Renderer vertex layout must match shader layout.");

	if(device == nullptr) return false;
	const char* vertexShaderPath = nullptr;
	const char* pixelShaderPath = nullptr;
	const char* shadowVertexShaderPath = nullptr;
	const RHI::RendererShaderPaths defaultShaderPaths = device->GetDefaultRendererShaderPaths();
	if(!ResolveRendererShaderPaths(
		config,
		defaultShaderPaths,
		vertexShaderPath,
		pixelShaderPath,
		shadowVertexShaderPath))
	{
		return false;
	}

	m_config = config;
	m_vertexShaderSource = ReadBinaryFile(vertexShaderPath);
	m_pixelShaderSource = ReadBinaryFile(pixelShaderPath);
	m_shadowVertexShaderSource.clear();
	if(m_config.enableShadows && shadowVertexShaderPath != nullptr)
	{
		m_shadowVertexShaderSource = ReadBinaryFile(shadowVertexShaderPath);
	}
	BuildRenderPassPlan();
	BuildPipelineStates(device);
	return m_texturedTrianglePipeline != nullptr;
}

void Renderer::SetViewProjection(const Math::float4x4& viewProjection)
{
	m_config.viewProjectionMatrix = viewProjection;
}

void Renderer::SetCameraPosition(const Math::float3& cameraPosition)
{
	m_config.cameraPosition = cameraPosition;
}

void Renderer::SetDirectionalLight(const Math::float3& direction, const Math::float3& color, float intensity)
{
	m_config.directionalLightDirection = direction;
	m_config.directionalLightColor = color;
	m_config.directionalLightIntensity = intensity;
}

void Renderer::SetAmbientLight(const Math::float3& color, float intensity)
{
	m_config.ambientColor = color;
	m_config.ambientIntensity = intensity;
}

void Renderer::SetPBR(const PBRDesc& pbr)
{
	m_config.pbr = pbr;
}

void Renderer::SetEnvironmentLight(const EnvironmentDesc& environment)
{
	m_config.environment = environment;
}

void Renderer::Shutdown(RHI::IDevice* device)
{
	if(device == nullptr) return;

	for(SceneTextureState& textureState : m_textureStates)
	{
		if(textureState.texture != nullptr)
		{
			device->DestroyTexture(textureState.texture);
			textureState.texture = nullptr;
		}
	}

	m_textureStates.clear();
	m_materialStates.clear();
	for(SceneMeshState& meshState : m_meshStates)
	{
		DestroyMeshState(device, meshState);
	}
	m_meshStates.clear();
	m_vertexShaderSource.clear();
	m_pixelShaderSource.clear();
	m_shadowVertexShaderSource.clear();
	m_renderPasses.clear();

	if(m_lightingBuffer != nullptr)
	{
		device->DestroyBuffer(m_lightingBuffer);
		m_lightingBuffer = nullptr;
	}
	if(m_shadowMatrixBuffer != nullptr)
	{
		device->DestroyBuffer(m_shadowMatrixBuffer);
		m_shadowMatrixBuffer = nullptr;
	}

	if(m_texturedTrianglePipeline != nullptr)
	{
		device->DestroyPipelineState(m_texturedTrianglePipeline);
		m_texturedTrianglePipeline = nullptr;
	}
}

void Renderer::Render(const Scene& scene, RHI::IDevice* device)
{
	if(device == nullptr) return;

	PrepareSceneResources(scene, device);
	ExecuteRenderPasses(scene, device);
}

void Renderer::BuildPipelineStates(RHI::IDevice* device)
{
	RHI::GraphicsPipelineDesc desc = {};
	desc.vertexShader = m_vertexShaderSource.data();
	desc.vertexShaderSize = m_vertexShaderSource.size();
	desc.pixelShader = m_pixelShaderSource.data();
	desc.pixelShaderSize = m_pixelShaderSource.size();
	desc.renderTargetFormat = m_config.renderTargetFormat;
	desc.depthStencilFormat = m_config.depthStencilFormat;
	desc.depthEnable = m_config.depthStencilFormat != RHI::Format::Unknown;
	desc.wireframe = false;
	desc.enableShadowPass = IsShadowEnabled();
	desc.shadowMapResolution = m_config.shadowMap.resolution;
	desc.shadowVertexShader = m_shadowVertexShaderSource.empty() ? nullptr : m_shadowVertexShaderSource.data();
	desc.shadowVertexShaderSize = m_shadowVertexShaderSource.size();

	m_texturedTrianglePipeline = device->CreateGraphicsPipeline(desc);
}

void Renderer::BuildRenderPassPlan()
{
	m_renderPasses.clear();
	m_renderPasses.push_back(RenderPassDesc{
		RenderPassKind::Shadow,
		RenderPassWork::PrepareOnly,
		"Shadow",
		m_config.enableShadows && !m_shadowVertexShaderSource.empty()
	});
	m_renderPasses.push_back(RenderPassDesc{
		RenderPassKind::MainForward,
		RenderPassWork::Graphics,
		"MainForward",
		m_config.enableMainPass
	});
}

void Renderer::PrepareSceneResources(const Scene& scene, RHI::IDevice* device)
{
	const uint32_t textureCount = scene.GetTextureCount();
	const uint32_t materialCount = scene.GetMaterialCount();
	const uint32_t meshCount = scene.GetMeshCount();
	EnsureTextureStateCapacity(textureCount);
	EnsureMaterialStateCapacity(materialCount);
	EnsureMeshStateCapacity(meshCount);

	for(uint32_t textureIndex = 0; textureIndex < textureCount; ++textureIndex)
	{
		SceneTextureState& textureState = m_textureStates[textureIndex];
		const Core::Image& image = scene.GetTexture(static_cast<TextureID>(textureIndex));

		if(!image.IsValid()) continue;

		if(textureState.texture == nullptr)
		{
			RHI::TextureDesc textureDesc = {};
			textureDesc.width = image.GetWidth();
			textureDesc.height = image.GetHeight();
			textureDesc.depthOrArraySize = 1;
			textureDesc.mipLevels = 1;
			textureDesc.format = RHI::Format::R8G8B8A8_UNORM;
			textureDesc.usage = RHI::TextureUsage::ShaderResource;

			textureState.texture = device->CreateTexture(textureDesc);
			textureState.descriptorIndex = device->AllocateDescriptorSlot();
			if(textureState.texture != nullptr)
			{
				if(device->UpdateTexture(textureState.texture, image.GetPixels().data(), image.GetRowPitch()))
				{
					device->UpdateDescriptorSlot(textureState.descriptorIndex, textureState.texture);
				}
				else
				{
					device->DestroyTexture(textureState.texture);
					textureState.texture = nullptr;
					textureState.descriptorIndex = kInvalidDescriptorIndex;
				}
			}
		}
	}

	UpdateMaterialStates(scene);

	for(uint32_t meshIndex = 0; meshIndex < meshCount; ++meshIndex)
	{
		const dy::Mesh& mesh = scene.GetMesh(static_cast<MeshID>(meshIndex));
		if(mesh.vertices.empty()) continue;

		std::vector<RendererVertex> vertices = BuildRendererVertices(mesh);
		std::vector<uint32_t> indices = BuildRendererIndices(mesh);
		if(indices.empty()) continue;

		SceneMeshState& meshState = m_meshStates[meshIndex];
		const uint32_t vertexBytes = static_cast<uint32_t>(vertices.size() * sizeof(RendererVertex));
		const uint32_t indexBytes = static_cast<uint32_t>(indices.size() * sizeof(uint32_t));

		if(meshState.vertexBuffer == nullptr || meshState.indexBuffer == nullptr ||
			meshState.vertexBytes != vertexBytes || meshState.indexBytes != indexBytes)
		{
			DestroyMeshState(device, meshState);
			const RHI::BufferUsage vertexUsage = RHI::BufferUsage::Vertex | RHI::BufferUsage::Storage;
			const RHI::BufferUsage indexUsage = RHI::BufferUsage::Index | RHI::BufferUsage::Storage;
			meshState.vertexBuffer = device->CreateBuffer(RHI::BufferDesc{ vertexBytes, static_cast<uint32_t>(sizeof(RendererVertex)), vertexUsage });
			meshState.indexBuffer = device->CreateBuffer(RHI::BufferDesc{ indexBytes, static_cast<uint32_t>(sizeof(uint32_t)), indexUsage });
			meshState.vertexBytes = vertexBytes;
			meshState.indexBytes = indexBytes;
		}

		if(meshState.vertexBuffer == nullptr || meshState.indexBuffer == nullptr) continue;

		void* vertexData = meshState.vertexBuffer->Map(0, vertexBytes);
		if(vertexData != nullptr)
		{
			std::memcpy(vertexData, vertices.data(), vertexBytes);
			meshState.vertexBuffer->Unmap();
		}

		void* indexData = meshState.indexBuffer->Map(0, indexBytes);
		if(indexData != nullptr)
		{
			std::memcpy(indexData, indices.data(), indexBytes);
			meshState.indexBuffer->Unmap();
		}

		meshState.indexCount = static_cast<uint32_t>(indices.size());
	}

}

void Renderer::ExecuteRenderPasses(const Scene& scene, RHI::IDevice* device)
{
	for(const RenderPassDesc& pass : m_renderPasses)
	{
		if(!pass.enabled) continue;
		ExecuteRenderPass(pass, scene, device);
	}
}

void Renderer::ExecuteRenderPass(const RenderPassDesc& pass, const Scene& scene, RHI::IDevice* device)
{
	switch(pass.kind)
	{
	case RenderPassKind::Shadow:
		PrepareShadowPass(scene, device);
		break;
	case RenderPassKind::MainForward:
		PrepareMainForwardPass(scene, device);
		if(pass.work == RenderPassWork::Graphics)
		{
			RecordMainForwardPass(scene, device);
		}
		break;
	default:
		break;
	}
}

void Renderer::PrepareShadowPass(const Scene& scene, RHI::IDevice* device)
{
	UpdateShadowBuffer(scene, device);
}

void Renderer::PrepareMainForwardPass(const Scene& scene, RHI::IDevice* device)
{
	UpdateLightingBuffer(scene, device);
}

void Renderer::RecordMainForwardPass(const Scene& scene, RHI::IDevice* device)
{
	RHI::ICommandList* commandList = device->AcquireCommandList();
	RHI::ITexture* backBuffer = device->GetBackBuffer();
	if(commandList == nullptr || backBuffer == nullptr || m_texturedTrianglePipeline == nullptr) return;

	commandList->SetRenderTargets(1, &backBuffer, nullptr);
	commandList->ClearColor(backBuffer, m_config.clearColor.x, m_config.clearColor.y, m_config.clearColor.z, m_config.clearColor.w);
	commandList->BindGraphicsPipeline(m_texturedTrianglePipeline);
	commandList->BindGlobalDescriptorHeap();
	if(m_lightingBuffer != nullptr)
	{
		commandList->BindConstantBuffer(Layout::kLightingConstantBinding, m_lightingBuffer, 0, static_cast<uint32_t>(sizeof(Layout::RendererLightingConstants)));
	}
	if(m_shadowMatrixBuffer != nullptr)
	{
		commandList->BindConstantBuffer(Layout::kShadowMatrixBinding, m_shadowMatrixBuffer, 0, static_cast<uint32_t>(sizeof(Layout::RendererShadowConstants)));
	}

	const uint32_t entityCount = scene.GetEntityCount();
	for(uint32_t entityIndex = 0; entityIndex < entityCount; ++entityIndex)
	{
		const EntityID entity = static_cast<EntityID>(entityIndex);
		const MeshID meshId = scene.GetEntityMesh(entity);
		const MaterialID materialId = scene.GetEntityMaterial(entity);
		if(!IsValid(meshId) || !IsValid(materialId)) continue;

		const dy::Mesh& mesh = scene.GetMesh(meshId);
		(void)mesh;
		const SceneMeshState& meshState = m_meshStates[ToIndex(meshId)];
		if(meshState.vertexBuffer == nullptr || meshState.indexBuffer == nullptr || meshState.indexCount == 0u) continue;
		const uint32_t materialIndex = ToIndex(materialId);
		if(materialIndex >= m_materialStates.size()) continue;

		const Material& material = scene.GetMaterial(materialId);
		const SceneMaterialState& materialState = m_materialStates[materialIndex];
		const Transform& transform = scene.GetTransform(entity);
		const RenderFlags& renderFlags = scene.GetRenderFlags(entity);
		uint32_t textureFlags = materialState.textureFlags;
		if(renderFlags.receiveShadow) textureFlags |= Layout::kReceiveShadowFlag;
		if(renderFlags.castShadow) textureFlags |= Layout::kCastShadowFlag;

		RHI::GeometryBinding geometry = {};
		geometry.vertexBuffer = meshState.vertexBuffer;
		geometry.vertexStride = static_cast<uint32_t>(sizeof(RendererVertex));
		geometry.vertexOffset = 0;
		geometry.indexBuffer = meshState.indexBuffer;
		geometry.indexFormat = RHI::Format::R32_UINT;
		geometry.indexOffset = 0;
		commandList->BindGeometry(geometry);
		commandList->BindTexture(Layout::kBaseColorTextureBinding, materialState.textures[kMaterialBaseColorTextureSlot]);
		commandList->BindTexture(Layout::kMetallicRoughnessSamplerBinding, materialState.textures[kMaterialMetallicRoughnessTextureSlot]);
		commandList->BindTexture(Layout::kNormalSamplerBinding, materialState.textures[kMaterialNormalTextureSlot]);
		commandList->BindTexture(Layout::kOcclusionSamplerBinding, materialState.textures[kMaterialOcclusionTextureSlot]);
		commandList->BindTexture(Layout::kEmissiveSamplerBinding, materialState.textures[kMaterialEmissiveTextureSlot]);

		Layout::DrawConstants drawConstants = {};
		drawConstants.viewProjectionMatrix = m_config.viewProjectionMatrix;
		drawConstants.modelMatrix = transform.worldMatrix;
		drawConstants.drawMode = static_cast<float>(textureFlags);
		drawConstants.emissiveColor = material.emissiveColor;
		drawConstants.baseColor = material.baseColor;
		drawConstants.materialParams = Math::float4(
			material.metallicFactor,
			material.roughnessFactor,
			material.normalScale,
			material.occlusionStrength);

		commandList->SetPushConstants(sizeof(Layout::DrawConstants), &drawConstants);
		commandList->DrawIndexedInstanced(meshState.indexCount, 1, 0, 0, 0);
	}

	commandList->Close();

	std::array<RHI::ICommandList*, 1> commandLists = { commandList };
	device->Submit(commandLists.data(), static_cast<uint32_t>(commandLists.size()));
}

void Renderer::EnsureTextureStateCapacity(std::size_t textureCount)
{
	if(m_textureStates.size() < textureCount)
	{
		m_textureStates.resize(textureCount);
	}
}

void Renderer::EnsureMaterialStateCapacity(std::size_t materialCount)
{
	if(m_materialStates.size() < materialCount)
	{
		m_materialStates.resize(materialCount);
	}
}

void Renderer::EnsureMeshStateCapacity(std::size_t meshCount)
{
	if(m_meshStates.size() < meshCount)
	{
		m_meshStates.resize(meshCount);
	}
}

void Renderer::UpdateMaterialStates(const Scene& scene)
{
	const uint32_t materialCount = scene.GetMaterialCount();
	for(uint32_t materialIndex = 0; materialIndex < materialCount; ++materialIndex)
	{
		const Material& material = scene.GetMaterial(static_cast<MaterialID>(materialIndex));
		SceneMaterialState& materialState = m_materialStates[materialIndex];
		materialState.textures[kMaterialBaseColorTextureSlot] = ResolveTexture(material.baseColorTexture);
		materialState.textures[kMaterialMetallicRoughnessTextureSlot] = ResolveTexture(material.metallicRoughnessTexture);
		materialState.textures[kMaterialNormalTextureSlot] = ResolveTexture(material.normalTexture);
		materialState.textures[kMaterialOcclusionTextureSlot] = ResolveTexture(material.occlusionTexture);
		materialState.textures[kMaterialEmissiveTextureSlot] = ResolveTexture(material.emissiveTexture);

		uint32_t textureFlags = 0;
		if(materialState.textures[kMaterialBaseColorTextureSlot] != nullptr) textureFlags |= Layout::kBaseColorTextureFlag;
		if(materialState.textures[kMaterialMetallicRoughnessTextureSlot] != nullptr) textureFlags |= Layout::kMetallicRoughnessTextureFlag;
		if(materialState.textures[kMaterialNormalTextureSlot] != nullptr) textureFlags |= Layout::kNormalTextureFlag;
		if(materialState.textures[kMaterialOcclusionTextureSlot] != nullptr) textureFlags |= Layout::kOcclusionTextureFlag;
		if(materialState.textures[kMaterialEmissiveTextureSlot] != nullptr) textureFlags |= Layout::kEmissiveTextureFlag;
		materialState.textureFlags = textureFlags;
	}
}

RHI::ITexture* Renderer::ResolveTexture(TextureID textureId) const
{
	if(!IsValid(textureId)) return nullptr;
	const uint32_t textureIndex = ToIndex(textureId);
	if(textureIndex >= m_textureStates.size()) return nullptr;
	return m_textureStates[textureIndex].texture;
}

void Renderer::UpdateLightingBuffer(const Scene& scene, RHI::IDevice* device)
{
	if(m_lightingBuffer == nullptr)
	{
		m_lightingBuffer = device->CreateBuffer(RHI::BufferDesc{
			static_cast<uint32_t>(sizeof(Layout::RendererLightingConstants)),
			static_cast<uint32_t>(sizeof(Layout::RendererLightingConstants)),
			RHI::BufferUsage::Constant
		});
	}
	if(m_lightingBuffer == nullptr) return;

	const DirectionalLight* light = GetPrimaryDirectionalLight(scene);
	const PointLight* pointLight = GetPrimaryPointLight(scene);
	const Math::float3 lightDirection = light != nullptr ? light->direction : m_config.directionalLightDirection;
	const Math::float3 lightColor = light != nullptr ? light->color : m_config.directionalLightColor;
	const float lightIntensity = light != nullptr ? light->intensity : m_config.directionalLightIntensity;
	const bool castsShadow = pointLight != nullptr ? pointLight->castShadow : (light != nullptr ? light->castShadow : true);
	const float shadowStrength = pointLight != nullptr ? pointLight->shadowStrength : (light != nullptr ? light->shadowStrength : m_config.shadowStrength);
	const bool shadowsEnabled = IsShadowEnabled() && castsShadow;

	Layout::RendererLightingConstants lighting = {};
	lighting.cameraPosition = Math::float4(
		m_config.cameraPosition.x,
		m_config.cameraPosition.y,
		m_config.cameraPosition.z,
		shadowsEnabled ? shadowStrength : 0.0f);
	lighting.directionalLightDirection = Math::float4(
		lightDirection.x,
		lightDirection.y,
		lightDirection.z,
		shadowsEnabled ? 1.0f : 0.0f);
	lighting.directionalLightColor = Math::float4(
		lightColor.x,
		lightColor.y,
		lightColor.z,
		lightIntensity);
	lighting.ambientColor = Math::float4(
		m_config.ambientColor.x * m_config.environment.diffuseColor.x,
		m_config.ambientColor.y * m_config.environment.diffuseColor.y,
		m_config.ambientColor.z * m_config.environment.diffuseColor.z,
		m_config.ambientIntensity * m_config.environment.diffuseIntensity);
	lighting.shadowParams = Math::float4(
		m_config.shadowDepthBias,
		m_config.shadowSlopeBias,
		m_config.shadowNormalBias,
		static_cast<float>(m_config.shadowPcfRadius));
	lighting.pbrParams = Math::float4(
		m_config.pbr.minRoughness,
		m_config.pbr.ambientSpecularStrength,
		0.0f,
		0.0f);
	lighting.environmentColor = Math::float4(
		m_config.environment.specularColor.x,
		m_config.environment.specularColor.y,
		m_config.environment.specularColor.z,
		m_config.environment.specularIntensity);
	if(pointLight != nullptr)
	{
		lighting.pointLightPositionRange = Math::float4(
			pointLight->position.x,
			pointLight->position.y,
			pointLight->position.z,
			pointLight->range);
		lighting.pointLightColorIntensity = Math::float4(
			pointLight->color.x,
			pointLight->color.y,
			pointLight->color.z,
			pointLight->intensity);
	}

	void* data = m_lightingBuffer->Map(0, static_cast<uint32_t>(sizeof(lighting)));
	if(data != nullptr)
	{
		std::memcpy(data, &lighting, sizeof(lighting));
		m_lightingBuffer->Unmap();
	}
}

void Renderer::UpdateShadowBuffer(const Scene& scene, RHI::IDevice* device)
{
	if(m_shadowMatrixBuffer == nullptr)
	{
		m_shadowMatrixBuffer = device->CreateBuffer(RHI::BufferDesc{
			static_cast<uint32_t>(sizeof(Layout::RendererShadowConstants)),
			static_cast<uint32_t>(sizeof(Layout::RendererShadowConstants)),
			RHI::BufferUsage::Constant
		});
	}
	if(m_shadowMatrixBuffer == nullptr) return;

	const DirectionalLight* light = GetPrimaryDirectionalLight(scene);
	const PointLight* pointLight = GetPrimaryPointLight(scene);
	Math::float3 lightDirection = light != nullptr ? light->direction : m_config.directionalLightDirection;
	ShadowMapDesc shadowMap = m_config.shadowMap;
	const ShadowBounds bounds = IsShadowEnabled() && m_config.autoFitShadowMap ? ComputeShadowBounds(scene) : ShadowBounds{};
	if(pointLight != nullptr)
	{
		shadowMap.farPlane = std::max(shadowMap.farPlane, pointLight->range);
		lightDirection = NormalizeOr(pointLight->direction, Math::float3(0.0f, 0.0f, -1.0f));
		if(bounds.valid)
		{
			const Math::float3 center(
				(bounds.min.x + bounds.max.x) * 0.5f,
				(bounds.min.y + bounds.max.y) * 0.5f,
				(bounds.min.z + bounds.max.z) * 0.5f);
			lightDirection = NormalizeOr(Subtract(center, pointLight->position), lightDirection);
			shadowMap.sceneCenter = center;
		}
	}
	else if(IsShadowEnabled() && m_config.autoFitShadowMap)
	{
		if(bounds.valid)
		{
			shadowMap = FitDirectionalShadowMapToBounds(
				lightDirection,
				m_config.shadowMap,
				bounds.min,
				bounds.max,
				m_config.shadowBoundsPadding);
		}
	}

	Layout::RendererShadowConstants shadow = {};
	if(IsShadowEnabled() && pointLight != nullptr)
	{
		shadow.lightViewProjectionMatrix = ComputeSpotLightViewProj(pointLight->position, lightDirection, shadowMap);
	}
	else
	{
		shadow.lightViewProjectionMatrix = IsShadowEnabled()
			? ComputeDirectionalLightViewProj(lightDirection, shadowMap)
			: Math::float4x4::Identity();
	}

	void* data = m_shadowMatrixBuffer->Map(0, static_cast<uint32_t>(sizeof(shadow)));
	if(data != nullptr)
	{
		std::memcpy(data, &shadow, sizeof(shadow));
		m_shadowMatrixBuffer->Unmap();
	}
}

void Renderer::DestroyMeshState(RHI::IDevice* device, SceneMeshState& meshState)
{
	if(meshState.vertexBuffer != nullptr)
	{
		device->DestroyBuffer(meshState.vertexBuffer);
		meshState.vertexBuffer = nullptr;
	}
	if(meshState.indexBuffer != nullptr)
	{
		device->DestroyBuffer(meshState.indexBuffer);
		meshState.indexBuffer = nullptr;
	}
	meshState.vertexBytes = 0;
	meshState.indexBytes = 0;
	meshState.indexCount = 0;
}

bool Renderer::IsShadowEnabled() const
{
	return IsRenderPassEnabled(RenderPassKind::Shadow);
}

bool Renderer::IsRenderPassEnabled(RenderPassKind passKind) const
{
	for(const RenderPassDesc& pass : m_renderPasses)
	{
		if(pass.kind == passKind)
		{
			return pass.enabled;
		}
	}
	return false;
}
