#include "Renderer.h"

#include <array>
#include <fstream>
#include <stdexcept>
#include <string>

#include "RHI/ICommandList.h"
#include "RHI/IDevice.h"
#include "RHI/IPipelineState.h"
#include "RHI/ITexture.h"

using namespace dy;
using namespace dy::Graphics;

bool Renderer::Initialize(RHI::IDevice* device, const GraphicsPipelineFiles& mainPipeline, const RendererConfig& config)
{
	if(device == nullptr) return false;

	m_config = config;
	return BuildPipelineStates(device, mainPipeline);
}

void Renderer::Shutdown(RHI::IDevice* device)
{
	if(device == nullptr) return;

	m_drawPackets.clear();
	m_mainVertexShader.clear();
	m_mainPixelShader.clear();

	if(m_mainPipeline != nullptr)
	{
		device->DestroyPipelineState(m_mainPipeline);
		m_mainPipeline = nullptr;
	}
}

void Renderer::Render(const Scene& scene, RHI::IDevice* device)
{
	if(device == nullptr) return;

	BuildDrawPackets(scene);
	RecordScenePass(scene, device);
}

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

bool Renderer::BuildPipelineStates(RHI::IDevice* device, const GraphicsPipelineFiles& mainPipeline)
{
	if(device == nullptr) return false;
	if(mainPipeline.vertexShaderPath == nullptr || mainPipeline.pixelShaderPath == nullptr) return false;

	const std::string vertexShaderSource = ReadTextFile(mainPipeline.vertexShaderPath);
	const std::string pixelShaderSource = ReadTextFile(mainPipeline.pixelShaderPath);
	m_mainVertexShader.assign(vertexShaderSource.begin(), vertexShaderSource.end());
	m_mainPixelShader.assign(pixelShaderSource.begin(), pixelShaderSource.end());
	m_mainVertexShader.push_back('\0');
	m_mainPixelShader.push_back('\0');

	RHI::GraphicsPipelineDesc desc = {};
	desc.vertexShader = m_mainVertexShader.data();
	desc.vertexShaderSize = m_mainVertexShader.empty() ? 0u : m_mainVertexShader.size() - 1u;
	desc.pixelShader = m_mainPixelShader.data();
	desc.pixelShaderSize = m_mainPixelShader.empty() ? 0u : m_mainPixelShader.size() - 1u;
	desc.renderTargetFormat = mainPipeline.renderTargetFormat;
	desc.depthStencilFormat = mainPipeline.depthStencilFormat;
	desc.depthStencil.depthTestEnable = mainPipeline.depthTestEnable;
	desc.depthStencil.depthWriteEnable = mainPipeline.depthWriteEnable;
	desc.depthEnable = mainPipeline.depthTestEnable || mainPipeline.depthWriteEnable;
	desc.wireframe = desc.rasterizer.fillMode == RHI::FillMode::Wireframe;

	m_mainPipeline = device->CreateGraphicsPipeline(desc);
	return m_mainPipeline != nullptr;
}

void Renderer::BuildDrawPackets(const Scene& scene)
{
	m_drawPackets.clear();

	const uint32_t entityCount = static_cast<uint32_t>(scene.m_entityMeshes.size());
	for(uint32_t entityIndex = 0; entityIndex < entityCount; ++entityIndex)
	{
		if(entityIndex >= scene.m_entityMaterials.size() || entityIndex >= scene.m_entityTransforms.size()) continue;

		const MeshID meshId = scene.m_entityMeshes[entityIndex];
		const MaterialID materialId = scene.m_entityMaterials[entityIndex];
		if(!IsValid(meshId) || !IsValid(materialId)) continue;

		const uint32_t meshIndex = ToIndex(meshId);
		const uint32_t materialIndex = ToIndex(materialId);
		if(meshIndex >= scene.m_meshes.size() || materialIndex >= scene.m_materials.size()) continue;

		const MeshData& mesh = scene.m_meshes[meshIndex];
		const uint32_t vertexCount = mesh.indices.empty()
			? static_cast<uint32_t>(mesh.vertices.size())
			: static_cast<uint32_t>(mesh.indices.size());
		if(vertexCount == 0u) continue;

		m_drawPackets.push_back(DrawPacket{ meshIndex, materialIndex, entityIndex, vertexCount });
	}
}

void Renderer::RecordScenePass(const Scene& scene, RHI::IDevice* device)
{
	RHI::ICommandList* commandList = device->AcquireCommandList();
	RHI::ITexture* backBuffer = device->GetBackBuffer();
	if(commandList == nullptr || backBuffer == nullptr || m_mainPipeline == nullptr) return;

	commandList->SetRenderTargets(1, &backBuffer, nullptr);
	commandList->ClearColor(backBuffer, m_config.clearColor.x, m_config.clearColor.y, m_config.clearColor.z, m_config.clearColor.w);
	commandList->BindGraphicsPipeline(m_mainPipeline);
	commandList->BindGlobalDescriptorHeap();

	for(const DrawPacket& packet : m_drawPackets)
	{
		const MaterialData& material = scene.m_materials[packet.materialIndex];
		const Transform& transform = scene.m_entityTransforms[packet.transformIndex];

		DrawConstants drawConstants = {};
		drawConstants.worldMatrix = transform.worldMatrix;
		drawConstants.baseColor = material.baseColor;
		drawConstants.baseColorTextureIndex = IsValid(material.baseColorTex)
			? ToIndex(material.baseColorTex)
			: kInvalidDescriptorIndex;

		commandList->SetPushConstants(sizeof(DrawConstants), &drawConstants);
		commandList->DrawInstanced(packet.vertexCount, 1, 0, 0);
	}

	commandList->Close();

	std::array<RHI::ICommandList*, 1> commandLists = { commandList };
	device->Submit(commandLists.data(), static_cast<uint32_t>(commandLists.size()));
}
