#include "Graphics/Renderer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
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

namespace
{
	constexpr uint32_t kBaseColorTextureBinding = 0;
	constexpr uint32_t kMetallicRoughnessTextureBinding = 6;
	constexpr uint32_t kNormalTextureBinding = 7;
	constexpr uint32_t kOcclusionTextureBinding = 8;
	constexpr uint32_t kEmissiveTextureBinding = 9;

	constexpr uint32_t kBaseColorTextureFlag = 1u << 0;
	constexpr uint32_t kMetallicRoughnessTextureFlag = 1u << 1;
	constexpr uint32_t kNormalTextureFlag = 1u << 2;
	constexpr uint32_t kOcclusionTextureFlag = 1u << 3;
	constexpr uint32_t kEmissiveTextureFlag = 1u << 4;
	constexpr uint32_t kReceiveShadowFlag = 1u << 5;
	constexpr uint32_t kCastShadowFlag = 1u << 6;

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
		const RendererConfig& config,
		const char*& vertexShaderPath,
		const char*& pixelShaderPath,
		const char*& shadowVertexShaderPath,
		std::string& defaultVertexShaderPath,
		std::string& defaultPixelShaderPath,
		std::string& defaultShadowVertexShaderPath)
	{
		vertexShaderPath = config.vertexShaderPath;
		pixelShaderPath = config.pixelShaderPath;
		shadowVertexShaderPath = config.shadowVertexShaderPath;

#if defined(ENABLE_VULKAN) && defined(DY_VULKAN_SHADER_DIR)
		if(vertexShaderPath == nullptr)
		{
			defaultVertexShaderPath = std::string(DY_VULKAN_SHADER_DIR) + "/triangle.vert.spv";
			vertexShaderPath = defaultVertexShaderPath.c_str();
		}
		if(pixelShaderPath == nullptr)
		{
			defaultPixelShaderPath = std::string(DY_VULKAN_SHADER_DIR) + "/triangle.frag.spv";
			pixelShaderPath = defaultPixelShaderPath.c_str();
		}
		if(config.enableShadows && shadowVertexShaderPath == nullptr)
		{
			defaultShadowVertexShaderPath = std::string(DY_VULKAN_SHADER_DIR) + "/triangle_shadow.vert.spv";
			shadowVertexShaderPath = defaultShadowVertexShaderPath.c_str();
		}
#else
		(void)defaultVertexShaderPath;
		(void)defaultPixelShaderPath;
		(void)defaultShadowVertexShaderPath;
#endif

		return vertexShaderPath != nullptr && pixelShaderPath != nullptr;
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
}

bool Renderer::Initialize(RHI::IDevice* device, const RendererConfig& config)
{
	static_assert(offsetof(DrawConstants, firstIndex) == 132u, "Renderer draw metadata must match Vulkan backend metadata offset.");
	static_assert(offsetof(DrawConstants, baseColor) == 160u, "Renderer material constants must fit after backend metadata.");
	static_assert(sizeof(DrawConstants) == 192u, "Renderer draw constants must match Vulkan push constant range.");

	if(device == nullptr) return false;
	const char* vertexShaderPath = nullptr;
	const char* pixelShaderPath = nullptr;
	const char* shadowVertexShaderPath = nullptr;
	std::string defaultVertexShaderPath;
	std::string defaultPixelShaderPath;
	std::string defaultShadowVertexShaderPath;
	if(!ResolveRendererShaderPaths(
		config,
		vertexShaderPath,
		pixelShaderPath,
		shadowVertexShaderPath,
		defaultVertexShaderPath,
		defaultPixelShaderPath,
		defaultShadowVertexShaderPath))
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
	for(SceneMeshState& meshState : m_meshStates)
	{
		DestroyMeshState(device, meshState);
	}
	m_meshStates.clear();
	m_vertexShaderSource.clear();
	m_pixelShaderSource.clear();
	m_shadowVertexShaderSource.clear();

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
	RecordScenePass(scene, device);
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

void Renderer::PrepareSceneResources(const Scene& scene, RHI::IDevice* device)
{
	const uint32_t textureCount = scene.GetTextureCount();
	const uint32_t meshCount = scene.GetMeshCount();
	EnsureTextureStateCapacity(textureCount);
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

	UpdateLightingBuffer(device);
	UpdateShadowBuffer(device);
}

void Renderer::RecordScenePass(const Scene& scene, RHI::IDevice* device)
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
		commandList->BindConstantBuffer(1, m_lightingBuffer, 0, static_cast<uint32_t>(sizeof(RendererLightingConstants)));
	}
	if(m_shadowMatrixBuffer != nullptr)
	{
		commandList->BindConstantBuffer(3, m_shadowMatrixBuffer, 0, static_cast<uint32_t>(sizeof(RendererShadowConstants)));
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

		const Material& material = scene.GetMaterial(materialId);
		const Transform& transform = scene.GetTransform(entity);
		const RenderFlags& renderFlags = scene.GetRenderFlags(entity);
		auto resolveTexture = [this](TextureID textureId) -> RHI::ITexture*
		{
			if(!IsValid(textureId)) return nullptr;
			const uint32_t textureIndex = ToIndex(textureId);
			if(textureIndex >= m_textureStates.size()) return nullptr;
			return m_textureStates[textureIndex].texture;
		};

		RHI::ITexture* baseColorTexture = resolveTexture(material.baseColorTexture);
		RHI::ITexture* metallicRoughnessTexture = resolveTexture(material.metallicRoughnessTexture);
		RHI::ITexture* normalTexture = resolveTexture(material.normalTexture);
		RHI::ITexture* occlusionTexture = resolveTexture(material.occlusionTexture);
		RHI::ITexture* emissiveTexture = resolveTexture(material.emissiveTexture);

		uint32_t textureFlags = 0;
		if(baseColorTexture != nullptr) textureFlags |= kBaseColorTextureFlag;
		if(metallicRoughnessTexture != nullptr) textureFlags |= kMetallicRoughnessTextureFlag;
		if(normalTexture != nullptr) textureFlags |= kNormalTextureFlag;
		if(occlusionTexture != nullptr) textureFlags |= kOcclusionTextureFlag;
		if(emissiveTexture != nullptr) textureFlags |= kEmissiveTextureFlag;
		if(renderFlags.receiveShadow) textureFlags |= kReceiveShadowFlag;
		if(renderFlags.castShadow) textureFlags |= kCastShadowFlag;

		RHI::GeometryBinding geometry = {};
		geometry.vertexBuffer = meshState.vertexBuffer;
		geometry.vertexStride = static_cast<uint32_t>(sizeof(RendererVertex));
		geometry.vertexOffset = 0;
		geometry.indexBuffer = meshState.indexBuffer;
		geometry.indexFormat = RHI::Format::R32_UINT;
		geometry.indexOffset = 0;
		commandList->BindGeometry(geometry);
		commandList->BindTexture(kBaseColorTextureBinding, baseColorTexture);
		commandList->BindTexture(kMetallicRoughnessTextureBinding, metallicRoughnessTexture);
		commandList->BindTexture(kNormalTextureBinding, normalTexture);
		commandList->BindTexture(kOcclusionTextureBinding, occlusionTexture);
		commandList->BindTexture(kEmissiveTextureBinding, emissiveTexture);

		DrawConstants drawConstants = {};
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

		commandList->SetPushConstants(sizeof(DrawConstants), &drawConstants);
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

void Renderer::EnsureMeshStateCapacity(std::size_t meshCount)
{
	if(m_meshStates.size() < meshCount)
	{
		m_meshStates.resize(meshCount);
	}
}

void Renderer::UpdateLightingBuffer(RHI::IDevice* device)
{
	if(m_lightingBuffer == nullptr)
	{
		m_lightingBuffer = device->CreateBuffer(RHI::BufferDesc{
			static_cast<uint32_t>(sizeof(RendererLightingConstants)),
			static_cast<uint32_t>(sizeof(RendererLightingConstants)),
			RHI::BufferUsage::Constant
		});
	}
	if(m_lightingBuffer == nullptr) return;

	RendererLightingConstants lighting = {};
	lighting.cameraPosition = Math::float4(
		m_config.cameraPosition.x,
		m_config.cameraPosition.y,
		m_config.cameraPosition.z,
		IsShadowEnabled() ? m_config.shadowStrength : 0.0f);
	lighting.directionalLightDirection = Math::float4(
		m_config.directionalLightDirection.x,
		m_config.directionalLightDirection.y,
		m_config.directionalLightDirection.z,
		IsShadowEnabled() ? 1.0f : 0.0f);
	lighting.directionalLightColor = Math::float4(
		m_config.directionalLightColor.x,
		m_config.directionalLightColor.y,
		m_config.directionalLightColor.z,
		m_config.directionalLightIntensity);
	lighting.ambientColor = Math::float4(
		m_config.ambientColor.x,
		m_config.ambientColor.y,
		m_config.ambientColor.z,
		m_config.ambientIntensity);

	void* data = m_lightingBuffer->Map(0, static_cast<uint32_t>(sizeof(lighting)));
	if(data != nullptr)
	{
		std::memcpy(data, &lighting, sizeof(lighting));
		m_lightingBuffer->Unmap();
	}
}

void Renderer::UpdateShadowBuffer(RHI::IDevice* device)
{
	if(m_shadowMatrixBuffer == nullptr)
	{
		m_shadowMatrixBuffer = device->CreateBuffer(RHI::BufferDesc{
			static_cast<uint32_t>(sizeof(RendererShadowConstants)),
			static_cast<uint32_t>(sizeof(RendererShadowConstants)),
			RHI::BufferUsage::Constant
		});
	}
	if(m_shadowMatrixBuffer == nullptr) return;

	RendererShadowConstants shadow = {};
	shadow.lightViewProjectionMatrix = IsShadowEnabled()
		? ComputeDirectionalLightViewProj(m_config.directionalLightDirection, m_config.shadowMap)
		: Math::float4x4::Identity();

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
	return m_config.enableShadows && !m_shadowVertexShaderSource.empty();
}
