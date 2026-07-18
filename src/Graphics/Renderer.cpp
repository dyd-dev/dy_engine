#include "Graphics/Renderer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "Graphics/RenderPath.h"
#include "Graphics/Scene.h"
#include "Graphics/ShadowMath.h"
#include "Math/Math.h"
#include "RHI/IBuffer.h"
#include "RHI/IDevice.h"
#include "RHI/IPipelineState.h"
#include "RHI/ITexture.h"

using namespace dy;
using namespace dy::Graphics;

namespace Layout = dy::Graphics::RendererShaderLayout;

namespace
{
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
		if(size > 0) file.read(content.data(), size);
		return content;
	}

	// bindless 모드는 set=1 텍스처 배열을 인덱싱하는 별도 픽셀 셰이더 변형을 쓴다.
	// "mesh_ps.spv" -> "mesh_ps_bindless.spv" 처럼 확장자 앞에 _bindless 를 삽입.
	[[nodiscard]] std::string MakeBindlessVariantPath(const char* path)
	{
		std::string result = path;
		const size_t dot = result.find_last_of('.');
		if(dot == std::string::npos) return result + "_bindless";
		return result.substr(0, dot) + "_bindless" + result.substr(dot);
	}

	[[nodiscard]] bool ResolveRendererShaderPaths(
		const RendererDesc& config,
		const char*& vertexShaderPath,
		const char*& pixelShaderPath,
		const char*& shadowVertexShaderPath)
	{
		// 셰이더 경로는 앱이 RendererDesc로 제공한다(RHI 디바이스는 셰이더를 모른다).
		vertexShaderPath = config.vertexShaderPath;
		pixelShaderPath = config.pixelShaderPath;
		shadowVertexShaderPath = config.shadowVertexShaderPath;

#if defined(VULKAN_DEFAULT_RENDERER_VERTEX_SHADER_PATH)
		if(vertexShaderPath == nullptr) vertexShaderPath = VULKAN_DEFAULT_RENDERER_VERTEX_SHADER_PATH;
#endif
#if defined(VULKAN_DEFAULT_RENDERER_PIXEL_SHADER_PATH)
		if(pixelShaderPath == nullptr) pixelShaderPath = VULKAN_DEFAULT_RENDERER_PIXEL_SHADER_PATH;
#endif
#if defined(VULKAN_DEFAULT_RENDERER_SHADOW_VERTEX_SHADER_PATH)
		if(shadowVertexShaderPath == nullptr) shadowVertexShaderPath = VULKAN_DEFAULT_RENDERER_SHADOW_VERTEX_SHADER_PATH;
#endif

		return vertexShaderPath != nullptr && pixelShaderPath != nullptr && (!config.enableShadows || shadowVertexShaderPath != nullptr);
	}

	[[nodiscard]] const DirectionalLight* GetPrimaryDirectionalLight(const Scene& scene)
	{
		return scene.GetDirectionalLightCount() > 0 ? &scene.GetDirectionalLight(0) : nullptr;
	}

	[[nodiscard]] const PointLight* GetPrimaryPointLight(const Scene& scene)
	{
		return scene.GetPointLightCount() > 0 ? &scene.GetPointLight(0) : nullptr;
	}

	[[nodiscard]] Math::Bounds3 ComputeShadowBounds(const Scene& scene)
	{
		Math::Bounds3 bounds = {};
		const uint32_t entityCount = scene.GetEntityCount();
		for(uint32_t entityIndex = 0; entityIndex < entityCount; ++entityIndex)
		{
			const EntityID entity = static_cast<EntityID>(entityIndex);
			const RenderFlags& flags = scene.GetRenderFlags(entity);
			if(!flags.castShadow && !flags.receiveShadow) continue;

			const MeshID meshId = scene.GetEntityMesh(entity);
			if(!IsValid(meshId)) continue;

			const MeshData& mesh = scene.GetMesh(meshId);
			const Transform& transform = scene.GetTransform(entity);
			for(const Vertex& vertex : mesh.vertices)
			{
				bounds.Include(Math::TransformPoint(transform.worldMatrix, vertex.position));
			}
		}
		return bounds;
	}
}

Renderer::Renderer(RendererBindingMode bindingMode)
	: m_initialBindingMode(bindingMode)
{
	m_config.bindingMode = bindingMode;
	if(bindingMode == RendererBindingMode::Bindless)
	{
		m_config.enableBindlessTextures = true;
	}
}

Renderer::Renderer(const RendererDesc& desc)
	: m_config(desc)
	, m_initialBindingMode(desc.bindingMode)
	, m_hasInitialConfig(true)
{
	if(desc.bindingMode == RendererBindingMode::Bindless)
	{
		m_config.enableBindlessTextures = true;
	}
}

bool Renderer::Initialize(RHI::IDevice* device, const RendererDesc& config)
{
	static_assert(Layout::kPushConstantRangeSize == sizeof(Layout::DrawConstants), "Renderer draw constants size mismatch.");

	if(device == nullptr) return false;
	RendererDesc effectiveConfig = config;
	if(m_hasInitialConfig &&
		config.bindingMode == RendererBindingMode::PerDrawBind &&
		config.vertexShaderPath == nullptr &&
		config.pixelShaderPath == nullptr &&
		config.shadowVertexShaderPath == nullptr)
	{
		effectiveConfig = m_config;
	}

	const char* vertexShaderPath = nullptr;
	const char* pixelShaderPath = nullptr;
	const char* shadowVertexShaderPath = nullptr;
	if(!ResolveRendererShaderPaths(effectiveConfig, vertexShaderPath, pixelShaderPath, shadowVertexShaderPath))
	{
		return false;
	}

	m_config = effectiveConfig;
	if(effectiveConfig.bindingMode == RendererBindingMode::PerDrawBind && m_initialBindingMode != RendererBindingMode::PerDrawBind)
	{
		m_config.bindingMode = m_initialBindingMode;
	}
	if(m_config.bindingMode == RendererBindingMode::Bindless)
	{
		m_config.enableBindlessTextures = true;
	}

	m_vertexShaderSource = ReadBinaryFile(vertexShaderPath);
	// 풀 PBR 글로벌-힙 인덱싱 픽셀 셰이더 변형(_bindless)이 존재하면 바인딩 모드와 무관하게 그것을 사용하고
	// 텍스처 샘플링을 힙/배열 인덱싱으로 통일한다. 이렇게 하면 enableBindlessTextures=true 가 되어
	// per-draw/batched 경로도 머티리얼 텍스처 개별 바인딩(BindMaterialTextures)을 건너뛰고, 5종 텍스처를
	// push constant 의 디스크립터 인덱스로 직접 샘플한다(= VK 와 동일한 풀 PBR). 변형이 없는 예제(단일
	// 베이스컬러 셰이더만 제공)는 기존 per-texture 바인딩 경로를 그대로 쓴다.
	std::string resolvedPixelShaderPath = pixelShaderPath;
	const std::string indexedPixelShaderPath = MakeBindlessVariantPath(pixelShaderPath);
	std::ifstream indexedPixelShaderFile(indexedPixelShaderPath, std::ios::binary);
	if(m_config.bindingMode == RendererBindingMode::Bindless && indexedPixelShaderFile.good())
	{
		resolvedPixelShaderPath = indexedPixelShaderPath;
		m_config.enableBindlessTextures = true;
	}
	m_pixelShaderSource = ReadBinaryFile(resolvedPixelShaderPath.c_str());
	m_shadowVertexShaderSource.clear();
	if(m_config.enableShadows && shadowVertexShaderPath != nullptr)
	{
		m_shadowVertexShaderSource = ReadBinaryFile(shadowVertexShaderPath);
	}

	m_clipYFlip = device->RequiresClipSpaceYFlip();

	BuildRenderPassPlan();
	BuildPipelineStates(device);
	m_path = CreateRenderPath(m_config.bindingMode);
	return m_pipeline != nullptr && m_path != nullptr;
}

void Renderer::SetCamera(const CameraDesc& camera)
{
	const Math::float4x4 view = Math::LookAtRH(camera.eye, camera.target, camera.up);
	Math::float4x4 proj = camera.orthographic
		? Math::OrthographicRH_ZO(camera.orthoWidth, camera.orthoHeight, camera.nearPlane, camera.farPlane)
		: Math::PerspectiveRH_ZO(camera.fovYRadians, camera.aspect, camera.nearPlane, camera.farPlane);

	if(m_clipYFlip)
	{
		proj.m[5] = -proj.m[5];
	}

	m_config.viewProjectionMatrix = proj * view;
	m_config.cameraPosition = camera.eye;
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

	if(m_path != nullptr) m_path->Shutdown(device);
	m_path.reset();

	m_gpuScene.Shutdown(device);
	m_materialStates.clear();
	m_vertexShaderSource.clear();
	m_pixelShaderSource.clear();
	m_shadowVertexShaderSource.clear();
	m_renderPasses.clear();

	if(m_lightingBuffer != nullptr)
	{
		device->DestroyBuffer(m_lightingBuffer);
		m_lightingBuffer = nullptr;
	}
	if(m_depthStencilTarget != nullptr)
	{
		device->DestroyTexture(m_depthStencilTarget);
		m_depthStencilTarget = nullptr;
	}
	if(m_shadowDepthTarget != nullptr)
	{
		device->DestroyTexture(m_shadowDepthTarget);
		m_shadowDepthTarget = nullptr;
		m_shadowDescriptorIndex = 0xFFFFFFFFu;
	}
	if(m_shadowMatrixBuffer != nullptr)
	{
		device->DestroyBuffer(m_shadowMatrixBuffer);
		m_shadowMatrixBuffer = nullptr;
	}
	if(m_shadowPipeline != nullptr)
	{
		device->DestroyPipelineState(m_shadowPipeline);
		m_shadowPipeline = nullptr;
	}
	if(m_pipeline != nullptr)
	{
		device->DestroyPipelineState(m_pipeline);
		m_pipeline = nullptr;
	}
}

void Renderer::Render(const Scene& scene, RHI::IDevice* device)
{
	if(device == nullptr || m_path == nullptr) return;

	// 공유 준비: 텍스처 GPU 레지던시 + 머티리얼 상태(모든 전략 공통).
	m_gpuScene.SyncTextures(scene, device);
	EnsureMaterialStateCapacity(scene.GetMaterialCount());
	UpdateMaterialStates(scene);

	// 전략별 지오메트리/드로우 리소스 준비.
	RenderPathContext context = {};
	context.config = &m_config;
	context.pipeline = m_pipeline;
	context.gpuScene = &m_gpuScene;
	context.materialStates = &m_materialStates;
	EnsureDepthStencilTarget(device);
	context.depthStencil = m_depthStencilTarget;
	m_path->PrepareResources(scene, device, context);

	UpdateShadowBuffer(scene, device);
	if(m_shadowPipeline != nullptr)
	{
		EnsureShadowDepthTarget(device);
	}
	UpdateLightingBuffer(scene, device);
	context.lightingBuffer = m_lightingBuffer;
	context.shadowMatrixBuffer = m_shadowMatrixBuffer;

	if(m_shadowDepthTarget != nullptr && m_shadowPipeline != nullptr)
	{
		context.shadowPipeline = m_shadowPipeline;
		context.shadowDepth = m_shadowDepthTarget;
	}

	// 정식화된 패스 루프: 모든 전략이 동일 경로를 거친다.
	for(const RenderPassDesc& pass : m_renderPasses)
	{
		if(!pass.enabled) continue;
		if(pass.kind == RenderPassKind::MainForward && pass.work == RenderPassWork::Graphics)
		{
			m_path->RecordMainPass(scene, device, context);
		}
	}
}

void Renderer::BuildPipelineStates(RHI::IDevice* device)
{
	// 렌더 타깃 포맷은 실제 백버퍼에서 파생한다(단일 진실원). 이래야 PSO 가 실제 타깃과
	// 항상 일치하고, 백엔드별 스왑체인 포맷 불일치(감마 차이)가 생기지 않는다.
	if(RHI::ITexture* backBuffer = device->GetBackBuffer())
	{
		if(backBuffer->GetFormat() != RHI::Format::Unknown)
		{
			m_config.renderTargetFormat = backBuffer->GetFormat();
		}
	}

	RHI::GraphicsPipelineDesc desc = {};
	desc.vertexShader = m_vertexShaderSource.data();
	desc.vertexShaderSize = m_vertexShaderSource.size();
	desc.pixelShader = m_pixelShaderSource.data();
	desc.pixelShaderSize = m_pixelShaderSource.size();
	desc.renderTargetFormat = m_config.renderTargetFormat;
	desc.depthStencilFormat = m_config.depthStencilFormat;
	desc.depthEnable = m_config.depthStencilFormat != RHI::Format::Unknown;
	desc.wireframe = false;
	desc.enableBindlessTextures = m_config.enableBindlessTextures;

	m_pipeline = device->CreateGraphicsPipeline(desc);

	if(IsShadowEnabled() && !m_shadowVertexShaderSource.empty())
	{
		const RHI::Format shadowFormat = m_config.depthStencilFormat != RHI::Format::Unknown
			? m_config.depthStencilFormat
			: RHI::Format::D32_FLOAT;

		RHI::GraphicsPipelineDesc shadowDesc = {};
		shadowDesc.vertexShader = m_shadowVertexShaderSource.data();
		shadowDesc.vertexShaderSize = m_shadowVertexShaderSource.size();
		shadowDesc.pixelShader = nullptr;
		shadowDesc.pixelShaderSize = 0;
		shadowDesc.renderTargetFormat = RHI::Format::Unknown;
		shadowDesc.depthStencilFormat = shadowFormat;
		shadowDesc.depthEnable = true;
		shadowDesc.enableBindlessTextures = m_config.enableBindlessTextures;
		shadowDesc.depthBiasSlope = 1.75f;

		m_shadowPipeline = device->CreateGraphicsPipeline(shadowDesc);
	}
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

void Renderer::EnsureDepthStencilTarget(RHI::IDevice* device)
{
	if(device == nullptr) return;

	if(m_config.depthStencilFormat == RHI::Format::Unknown)
	{
		if(m_depthStencilTarget != nullptr)
		{
			device->DestroyTexture(m_depthStencilTarget);
			m_depthStencilTarget = nullptr;
		}
		return;
	}

	RHI::ITexture* backBuffer = device->GetBackBuffer();
	if(backBuffer == nullptr || backBuffer->GetWidth() == 0u || backBuffer->GetHeight() == 0u) return;

	const bool recreate =
		m_depthStencilTarget == nullptr ||
		m_depthStencilTarget->GetWidth() != backBuffer->GetWidth() ||
		m_depthStencilTarget->GetHeight() != backBuffer->GetHeight() ||
		m_depthStencilTarget->GetFormat() != m_config.depthStencilFormat;

	if(!recreate) return;

	if(m_depthStencilTarget != nullptr)
	{
		device->DestroyTexture(m_depthStencilTarget);
		m_depthStencilTarget = nullptr;
	}

	RHI::TextureDesc depthDesc = {};
	depthDesc.width = backBuffer->GetWidth();
	depthDesc.height = backBuffer->GetHeight();
	depthDesc.depthOrArraySize = 1;
	depthDesc.mipLevels = 1;
	depthDesc.format = m_config.depthStencilFormat;
	depthDesc.usage = RHI::TextureUsage::DepthStencil;
	m_depthStencilTarget = device->CreateTexture(depthDesc);
}

void Renderer::EnsureShadowDepthTarget(RHI::IDevice* device)
{
	if(device == nullptr || m_shadowPipeline == nullptr) return;

	const uint32_t resolution = m_config.shadowMap.resolution;
	if(resolution == 0u) return;
	if(m_shadowDepthTarget != nullptr &&
		m_shadowDepthTarget->GetWidth() == resolution &&
		m_shadowDepthTarget->GetHeight() == resolution)
	{
		return;
	}

	if(m_shadowDepthTarget != nullptr)
	{
		device->DestroyTexture(m_shadowDepthTarget);
		m_shadowDepthTarget = nullptr;
		m_shadowDescriptorIndex = 0xFFFFFFFFu;
	}

	const RHI::Format shadowFormat = m_config.depthStencilFormat != RHI::Format::Unknown
		? m_config.depthStencilFormat
		: RHI::Format::D32_FLOAT;

	RHI::TextureDesc shadowDesc = {};
	shadowDesc.width = resolution;
	shadowDesc.height = resolution;
	shadowDesc.depthOrArraySize = 1;
	shadowDesc.mipLevels = 1;
	shadowDesc.format = shadowFormat;
	shadowDesc.usage = RHI::TextureUsage::DepthStencil | RHI::TextureUsage::ShaderResource;
	m_shadowDepthTarget = device->CreateTexture(shadowDesc);
	if(m_shadowDepthTarget == nullptr) return;

	// 그림자 맵 SRV 를 글로벌 디스크립터 힙에 한 번 등록(메인 패스에서 샘플링).
	m_shadowDescriptorIndex = device->AllocateDescriptorSlot();
	if(m_shadowDescriptorIndex == RHI::INVALID_DESCRIPTOR_INDEX)
	{
		device->DestroyTexture(m_shadowDepthTarget);
		m_shadowDepthTarget = nullptr;
		return;
	}
	device->UpdateDescriptorSlot(m_shadowDescriptorIndex, m_shadowDepthTarget);
}

void Renderer::EnsureMaterialStateCapacity(std::size_t materialCount)
{
	if(m_materialStates.size() < materialCount)
	{
		m_materialStates.resize(materialCount);
	}
}

void Renderer::UpdateMaterialStates(const Scene& scene)
{
	const uint32_t materialCount = scene.GetMaterialCount();
	for(uint32_t materialIndex = 0; materialIndex < materialCount; ++materialIndex)
	{
		const MaterialDesc& material = scene.GetMaterial(static_cast<MaterialID>(materialIndex));
		SceneMaterialState& materialState = m_materialStates[materialIndex];
		materialState.textures[kMaterialBaseColorTextureSlot] = m_gpuScene.ResolveTexture(material.baseColorTexture);
		materialState.textures[kMaterialMetallicRoughnessTextureSlot] = m_gpuScene.ResolveTexture(material.metallicRoughnessTexture);
		materialState.textures[kMaterialNormalTextureSlot] = m_gpuScene.ResolveTexture(material.normalTexture);
		materialState.textures[kMaterialOcclusionTextureSlot] = m_gpuScene.ResolveTexture(material.occlusionTexture);
		materialState.textures[kMaterialEmissiveTextureSlot] = m_gpuScene.ResolveTexture(material.emissiveTexture);
		materialState.textureDescriptorIndices[kMaterialBaseColorTextureSlot] = m_gpuScene.ResolveTextureDescriptorIndex(material.baseColorTexture);
		materialState.textureDescriptorIndices[kMaterialMetallicRoughnessTextureSlot] = m_gpuScene.ResolveTextureDescriptorIndex(material.metallicRoughnessTexture);
		materialState.textureDescriptorIndices[kMaterialNormalTextureSlot] = m_gpuScene.ResolveTextureDescriptorIndex(material.normalTexture);
		materialState.textureDescriptorIndices[kMaterialOcclusionTextureSlot] = m_gpuScene.ResolveTextureDescriptorIndex(material.occlusionTexture);
		materialState.textureDescriptorIndices[kMaterialEmissiveTextureSlot] = m_gpuScene.ResolveTextureDescriptorIndex(material.emissiveTexture);

		uint32_t textureFlags = 0;
		const bool useBindless = m_config.enableBindlessTextures;
		auto hasTexture = [&](uint32_t slot)
		{
			if(materialState.textures[slot] == nullptr) return false;
			return !useBindless || materialState.textureDescriptorIndices[slot] != kInvalidDescriptorIndex;
		};
		if(hasTexture(kMaterialBaseColorTextureSlot)) textureFlags |= Layout::kBaseColorTextureFlag;
		if(hasTexture(kMaterialMetallicRoughnessTextureSlot)) textureFlags |= Layout::kMetallicRoughnessTextureFlag;
		if(hasTexture(kMaterialNormalTextureSlot)) textureFlags |= Layout::kNormalTextureFlag;
		if(hasTexture(kMaterialOcclusionTextureSlot)) textureFlags |= Layout::kOcclusionTextureFlag;
		if(hasTexture(kMaterialEmissiveTextureSlot)) textureFlags |= Layout::kEmissiveTextureFlag;
		materialState.textureFlags = textureFlags;
	}
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
	const bool shadowsEnabled =
		IsShadowEnabled() &&
		m_shadowPipeline != nullptr &&
		m_shadowDepthTarget != nullptr &&
		castsShadow;

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
	lighting.directionalLightColor = Math::float4(lightColor.x, lightColor.y, lightColor.z, lightIntensity);
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
	lighting.pbrParams = Math::float4(m_config.pbr.minRoughness, m_config.pbr.ambientSpecularStrength, 0.0f, 0.0f);
	lighting.environmentColor = Math::float4(
		m_config.environment.specularColor.x,
		m_config.environment.specularColor.y,
		m_config.environment.specularColor.z,
		m_config.environment.specularIntensity);
	if(pointLight != nullptr)
	{
		lighting.pointLightPositionRange = Math::float4(
			pointLight->position.x, pointLight->position.y, pointLight->position.z, pointLight->range);
		lighting.pointLightColorIntensity = Math::float4(
			pointLight->color.x, pointLight->color.y, pointLight->color.z, pointLight->intensity);
	}

	void* data = m_lightingBuffer->Map(0);
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
	const Math::Bounds3 bounds = IsShadowEnabled() && m_config.autoFitShadowMap ? ComputeShadowBounds(scene) : Math::Bounds3{};
	if(pointLight != nullptr)
	{
		shadowMap.farPlane = std::max(shadowMap.farPlane, pointLight->range);
		lightDirection = NormalizeOr(pointLight->direction, Math::float3(0.0f, 0.0f, -1.0f));
		if(bounds.valid)
		{
			const Math::float3 center = bounds.Center();
			lightDirection = NormalizeOr(center - pointLight->position, lightDirection);
			shadowMap.sceneCenter = center;
		}
	}
	else if(IsShadowEnabled() && m_config.autoFitShadowMap)
	{
		if(bounds.valid)
		{
			shadowMap = FitDirectionalShadowMapToBounds(
				lightDirection, m_config.shadowMap, bounds.min, bounds.max, m_config.shadowBoundsPadding);
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

	void* data = m_shadowMatrixBuffer->Map(0);
	if(data != nullptr)
	{
		std::memcpy(data, &shadow, sizeof(shadow));
		m_shadowMatrixBuffer->Unmap();
	}
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
