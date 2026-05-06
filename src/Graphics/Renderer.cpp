#include "Graphics/Renderer.h"

#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include "Graphics/Scene.h"
#include "RHI/ICommandList.h"
#include "RHI/IDevice.h"
#include "RHI/IPipelineState.h"
#include "RHI/ITexture.h"

using namespace dy;
using namespace dy::Graphics;

namespace
{
	[[nodiscard]] std::string ReadTextFile(const char* filepath)
	{
		std::ifstream file(filepath, std::ios::binary);
		if(!file.is_open())
		{
			throw std::runtime_error(std::string("Failed to open shader file: ") + filepath);
		}

		file.seekg(0, std::ios::end);
		const std::streamoff size = file.tellg();
		file.seekg(0, std::ios::beg);

		std::string content(static_cast<size_t>(size), '\0');
		if(size > 0)
		{
			file.read(content.data(), size);
		}

		return content;
	}
}

bool Renderer::Initialize(RHI::IDevice* device, const RendererConfig& config)
{
	if(device == nullptr) return false;
	if(config.vertexShaderPath == nullptr || config.pixelShaderPath == nullptr) return false;

	m_config = config;
	const std::string vertexShaderSource = ReadTextFile(config.vertexShaderPath);
	const std::string pixelShaderSource = ReadTextFile(config.pixelShaderPath);
	m_vertexShaderSource.assign(vertexShaderSource.begin(), vertexShaderSource.end());
	m_pixelShaderSource.assign(pixelShaderSource.begin(), pixelShaderSource.end());
	m_vertexShaderSource.push_back('\0');
	m_pixelShaderSource.push_back('\0');
	BuildPipelineStates(device);
	return m_texturedTrianglePipeline != nullptr;
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
	m_vertexShaderSource.clear();
	m_pixelShaderSource.clear();

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
	desc.vertexShaderSize = m_vertexShaderSource.empty() ? 0u : m_vertexShaderSource.size() - 1u;
	desc.pixelShader = m_pixelShaderSource.data();
	desc.pixelShaderSize = m_pixelShaderSource.empty() ? 0u : m_pixelShaderSource.size() - 1u;
	desc.renderTargetFormat = m_config.renderTargetFormat;
	desc.depthStencilFormat = m_config.depthStencilFormat;
	desc.depthEnable = false;
	desc.wireframe = false;

	m_texturedTrianglePipeline = device->CreateGraphicsPipeline(desc);
}

void Renderer::PrepareSceneResources(const Scene& scene, RHI::IDevice* device)
{
	const uint32_t textureCount = scene.GetTextureCount();
	EnsureTextureStateCapacity(textureCount);

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
			textureDesc.initialData = image.GetPixels().data();
			textureDesc.initialDataRowPitch = image.GetRowPitch();

			textureState.texture = device->CreateTexture(textureDesc);
			textureState.descriptorIndex = device->AllocateDescriptorSlot();
			device->UpdateDescriptorSlot(textureState.descriptorIndex, textureState.texture);
		}
	}
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

	const uint32_t entityCount = scene.GetEntityCount();
	for(uint32_t entityIndex = 0; entityIndex < entityCount; ++entityIndex)
	{
		const EntityID entity = static_cast<EntityID>(entityIndex);
		const MeshID meshId = scene.GetEntityMesh(entity);
		const MaterialID materialId = scene.GetEntityMaterial(entity);
		if(!IsValid(meshId) || !IsValid(materialId)) continue;

		const Mesh& mesh = scene.GetMesh(meshId);
		const uint32_t vertexCount = mesh.indices.empty()
			? static_cast<uint32_t>(mesh.vertices.size())
			: static_cast<uint32_t>(mesh.indices.size());
		if(vertexCount == 0u) continue;

		const Material& material = scene.GetMaterial(materialId);
		const Transform& transform = scene.GetTransform(entity);

		DrawConstants drawConstants = {};
		drawConstants.worldMatrix = transform.worldMatrix;
		drawConstants.baseColor = material.baseColor;

		if(IsValid(material.baseColorTexture))
		{
			const SceneTextureState& textureState = m_textureStates[ToIndex(material.baseColorTexture)];
			drawConstants.baseColorTextureIndex = textureState.descriptorIndex;
		}

		commandList->SetPushConstants(sizeof(DrawConstants), &drawConstants);
		commandList->DrawInstanced(vertexCount, 1, 0, 0);
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
