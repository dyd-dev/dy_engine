#include "Graphics/Renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "Graphics/Private/DrawData.h"
#include "Graphics/Private/CanvasRenderer.h"
#include "Graphics/Private/FrameCapture.h"
#include "Graphics/Private/LightingData.h"
#include <stdexcept>
#include "Graphics/Private/RendererShaderLayout.h"
#include "Graphics/Private/StockShaderAssets.h"
#include "Graphics/Private/TextureCache.h"
#include "Graphics/Camera.h"
#include "Graphics/Scene.h"
#include "Math/Math.h"
#include "RHI/Buffer.h"
#include "RHI/ICommandList.h"
#include "RHI/IDevice.h"
#include "RHI/Pipeline.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"

using namespace dy;
using namespace dy::Graphics;

namespace Layout = dy::Graphics::Private::RendererShaderLayout;

namespace
{
	[[nodiscard]] RHI::Format ToRHIFormat(ColorFormat format)
	{
		switch(format)
		{
		case ColorFormat::RGBA8Unorm: return RHI::Format::R8G8B8A8_UNORM;
		case ColorFormat::BGRA8Unorm: return RHI::Format::B8G8R8A8_UNORM;
		case ColorFormat::RGBA8UnormSrgb: return RHI::Format::R8G8B8A8_UNORM_SRGB;
		case ColorFormat::BGRA8UnormSrgb: return RHI::Format::B8G8R8A8_UNORM_SRGB;
		}
		return RHI::Format::Unknown;
	}

	[[nodiscard]] RHI::Format ToRHIFormat(DepthFormat format)
	{
		switch(format)
		{
		case DepthFormat::None: return RHI::Format::Unknown;
		case DepthFormat::D32Float: return RHI::Format::D32_FLOAT;
		case DepthFormat::D24UnormStencil8: return RHI::Format::D24_UNORM_S8_UINT;
		}
		return RHI::Format::Unknown;
	}

	[[nodiscard]] Math::float3 SelectShadowUpVector(const Math::float3& lightForward)
	{
		return std::abs(lightForward.z) > 0.95f
			? Math::float3(0.0f, 1.0f, 0.0f)
			: Math::float3(0.0f, 0.0f, 1.0f);
	}

	[[nodiscard]] Math::float4x4 ComputeDirectionalLightViewProj(
		const Math::float3& lightDirection,
		const ShadowMapDesc& desc)
	{
		const Math::float3 lightForward = Normalize(lightDirection);
		const Math::float3 lightOrigin(
			desc.sceneCenter.x + lightForward.x * desc.lightDistance,
			desc.sceneCenter.y + lightForward.y * desc.lightDistance,
			desc.sceneCenter.z + lightForward.z * desc.lightDistance);
		const Math::float4x4 view = Math::LookAtRH(
			lightOrigin,
			desc.sceneCenter,
			SelectShadowUpVector(lightForward));
		return Math::OrthographicRH_ZO(
			desc.orthoWidth,
			desc.orthoHeight,
			desc.nearPlane,
			desc.farPlane) * view;
	}

	[[nodiscard]] ShadowMapDesc FitDirectionalShadowMapToBounds(
		const Math::float3& lightDirection,
		const ShadowMapDesc& baseDesc,
		const Math::float3& boundsMin,
		const Math::float3& boundsMax,
		float padding)
	{
		ShadowMapDesc desc = baseDesc;
		if(boundsMin.x > boundsMax.x || boundsMin.y > boundsMax.y || boundsMin.z > boundsMax.z)
		{
			return desc;
		}

		padding = std::max(padding, 0.0f);
		const Math::float3 center(
			(boundsMin.x + boundsMax.x) * 0.5f,
			(boundsMin.y + boundsMax.y) * 0.5f,
			(boundsMin.z + boundsMax.z) * 0.5f);
		const Math::float3 halfExtent(
			(boundsMax.x - boundsMin.x) * 0.5f,
			(boundsMax.y - boundsMin.y) * 0.5f,
			(boundsMax.z - boundsMin.z) * 0.5f);
		const float radius = std::max(Length(halfExtent), 0.1f);

		const Math::float3 lightForward = Normalize(lightDirection);
		desc.sceneCenter = center;
		desc.lightDistance = std::max(radius + padding + desc.nearPlane, 0.5f);

		const Math::float3 lightOrigin(
			desc.sceneCenter.x + lightForward.x * desc.lightDistance,
			desc.sceneCenter.y + lightForward.y * desc.lightDistance,
			desc.sceneCenter.z + lightForward.z * desc.lightDistance);
		const Math::float4x4 view = Math::LookAtRH(
			lightOrigin,
			desc.sceneCenter,
			SelectShadowUpVector(lightForward));

		const Math::float3 corners[] = {
			Math::float3(boundsMin.x, boundsMin.y, boundsMin.z),
			Math::float3(boundsMax.x, boundsMin.y, boundsMin.z),
			Math::float3(boundsMin.x, boundsMax.y, boundsMin.z),
			Math::float3(boundsMax.x, boundsMax.y, boundsMin.z),
			Math::float3(boundsMin.x, boundsMin.y, boundsMax.z),
			Math::float3(boundsMax.x, boundsMin.y, boundsMax.z),
			Math::float3(boundsMin.x, boundsMax.y, boundsMax.z),
			Math::float3(boundsMax.x, boundsMax.y, boundsMax.z)
		};

		float minX = std::numeric_limits<float>::max();
		float minY = std::numeric_limits<float>::max();
		float minZ = std::numeric_limits<float>::max();
		float maxX = -std::numeric_limits<float>::max();
		float maxY = -std::numeric_limits<float>::max();
		float maxZ = -std::numeric_limits<float>::max();
		for(const Math::float3& corner : corners)
		{
			const Math::float3 lightSpace = Math::TransformPoint(view, corner);
			minX = std::min(minX, lightSpace.x);
			minY = std::min(minY, lightSpace.y);
			minZ = std::min(minZ, lightSpace.z);
			maxX = std::max(maxX, lightSpace.x);
			maxY = std::max(maxY, lightSpace.y);
			maxZ = std::max(maxZ, lightSpace.z);
		}

		desc.orthoWidth = std::max(maxX - minX + padding * 2.0f, 0.1f);
		desc.orthoHeight = std::max(maxY - minY + padding * 2.0f, 0.1f);
		const float depthRange = std::max(maxZ - minZ + padding * 2.0f, 0.1f);
		desc.farPlane = std::max(
			desc.nearPlane + depthRange + desc.lightDistance,
			desc.nearPlane + 0.1f);
		return desc;
	}

	[[nodiscard]] const DirectionalLight* GetPrimaryDirectionalLight(const Scene& scene)
	{
		const auto active = SelectActiveLightIndices(scene.DirectionalLights(), 1);
		return active.empty() ? nullptr : &scene.GetDirectionalLight(active.front());
	}

	[[nodiscard]] const PointLight* GetPrimaryPointLight(const Scene& scene)
	{
		const auto active = SelectActiveLightIndices(scene.PointLights(), 1);
		return active.empty() ? nullptr : &scene.GetPointLight(active.front());
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

struct Renderer::Impl
{
	explicit Impl(std::unique_ptr<RHI::IDevice> ownedDevice, const RendererDesc& desc)
		: device(std::move(ownedDevice))
		, config(desc)
	{
	}

	~Impl();

	[[nodiscard]] bool Initialize();
	[[nodiscard]] bool InitializeMesh();
	void Shutdown();
	bool Render(const Scene& scene, const Camera& camera, TextureAsset* readback);
	[[nodiscard]] bool BuildPipelineStates(RHI::IDevice* device);
	[[nodiscard]] bool CreateDefaultMaterialTextures(RHI::IDevice* device);
	[[nodiscard]] bool EnsureDepthStencilTarget(RHI::IDevice* device);
	[[nodiscard]] bool EnsureShadowDepthTarget(RHI::IDevice* device);
	void UpdateMaterialStates(const Scene& scene);
	[[nodiscard]] bool UpdateLightingBuffer(
		const Scene& scene,
		const Camera& camera,
		RHI::IDevice* device,
		RHI::ICommandList& commandList);
	[[nodiscard]] bool UpdateShadowBuffer(
		const Scene& scene,
		RHI::IDevice* device,
		RHI::ICommandList& commandList);

	std::unique_ptr<RHI::IDevice> device;
	RendererDesc config = {};
	std::unique_ptr<Private::CanvasRenderer> canvasRenderer;
	RHI::ShaderHandle vertexShader = nullptr;
	RHI::ShaderHandle fragmentShader = nullptr;
	RHI::ShaderHandle shadowVertexShader = nullptr;
	RHI::PipelineHandle pipeline = nullptr;
	RHI::PipelineHandle shadowPipeline = nullptr;
	RHI::TextureHandle depthStencilTarget = nullptr;
	RHI::TextureHandle shadowDepthTarget = nullptr;
	RHI::BufferHandle lightingBuffer = nullptr;
	RHI::BufferHandle shadowMatrixBuffer = nullptr;
	std::array<RHI::TextureHandle, 3> defaultMaterialTextures = {};
	RHI::ResourceState depthStencilState = RHI::ResourceState::Undefined;
	RHI::ResourceState shadowDepthState = RHI::ResourceState::Undefined;
	Private::TextureCache textureCache;
	std::vector<Private::SceneMaterialState> materialStates;
	Private::DrawData drawData;
	const Scene* currentScene = nullptr;
};

std::unique_ptr<Renderer> Renderer::Create(const void* windowHandle, const RendererDesc& desc)
{
    if(windowHandle == nullptr) return nullptr;
    return Create(std::unique_ptr<RHI::IDevice>(RHI::IDevice::Create(windowHandle)), desc);
}

std::unique_ptr<Renderer> Renderer::Create(std::unique_ptr<RHI::IDevice> device, const RendererDesc& desc)
{
    if(device == nullptr || desc.backBufferCount == 0 ||
        ToRHIFormat(desc.outputFormat) == RHI::Format::Unknown ||
        (desc.enableShadows && desc.shadowFormat == DepthFormat::None))
        return nullptr;
    auto impl = std::make_unique<Impl>(std::move(device), desc);
    if(!impl->Initialize()) return nullptr;
    return std::unique_ptr<Renderer>(new Renderer(std::move(impl)));
}

Renderer::Renderer(std::unique_ptr<Impl> impl)
	: m_impl(std::move(impl))
{
}

Renderer::~Renderer() = default;

bool Renderer::Render(const Scene& scene, const Camera& camera, TextureAsset* readback)
{
    if(readback && !m_impl->config.allowReadback)
        throw std::invalid_argument("Renderer readback was not enabled.");
    return m_impl->Render(scene, camera, readback);
}

bool Renderer::Render(const Canvas& canvas, TextureAsset* readback)
{
    if(readback && !m_impl->config.allowReadback)
        throw std::invalid_argument("Renderer readback was not enabled.");
    return m_impl->canvasRenderer->Render(canvas, readback);
}

Renderer::Impl::~Impl()
{
	Shutdown();
}

bool Renderer::Impl::Initialize()
{
	RHI::IDevice* nativeDevice = device.get();
	if(nativeDevice == nullptr) return false;

	RHI::SwapchainDesc swapchainDesc = {};
	swapchainDesc.format = ToRHIFormat(config.outputFormat);
	swapchainDesc.minimumImageCount = config.backBufferCount;
	swapchainDesc.allowReadback = config.allowReadback;
	swapchainDesc.presentMode = config.vsync ? RHI::PresentMode::Fifo : RHI::PresentMode::Immediate;
	if(!nativeDevice->CreateSwapchain(swapchainDesc)) return false;

	RHI::TextureHandle backBuffer = nativeDevice->GetBackBuffer();
	if(backBuffer == nullptr || backBuffer->GetDesc().format == RHI::Format::Unknown) return false;
	if(backBuffer->GetDesc().format != swapchainDesc.format)
		return false;

	canvasRenderer = std::make_unique<Private::CanvasRenderer>(*device, config.canvasShaders);
    return true;
}

bool Renderer::Impl::InitializeMesh()
{
    if(pipeline != nullptr) return true;
    RHI::IDevice* nativeDevice = device.get();
    const ShaderAssets& custom = config.meshShaders;
    const ShaderAssets stockShaders = custom.vertex.binary || custom.fragment.binary || custom.shadowVertex.binary
        ? custom : GetMeshShaderAssets(config.enableShadows);

	vertexShader = nativeDevice->CreateShader(RHI::ShaderDesc{
		RHI::ShaderStage::Vertex,
		stockShaders.vertex.entryPoint,
		stockShaders.vertex.binary,
		stockShaders.vertex.binarySize });
	fragmentShader = nativeDevice->CreateShader(RHI::ShaderDesc{
		RHI::ShaderStage::Fragment,
		stockShaders.fragment.entryPoint,
		stockShaders.fragment.binary,
		stockShaders.fragment.binarySize });
	if(config.enableShadows)
	{
		shadowVertexShader = nativeDevice->CreateShader(RHI::ShaderDesc{
			RHI::ShaderStage::Vertex,
			stockShaders.shadowVertex.entryPoint,
			stockShaders.shadowVertex.binary,
			stockShaders.shadowVertex.binarySize });
	}
	if(vertexShader == nullptr || fragmentShader == nullptr ||
		(config.enableShadows && shadowVertexShader == nullptr)) return false;
	if(!BuildPipelineStates(nativeDevice) || !CreateDefaultMaterialTextures(nativeDevice)) return false;
	return EnsureDepthStencilTarget(nativeDevice) && EnsureShadowDepthTarget(nativeDevice);
}

void Renderer::Impl::Shutdown()
{
	RHI::IDevice* nativeDevice = device.get();
	if(nativeDevice == nullptr) return;

	canvasRenderer.reset();
	drawData.Shutdown(nativeDevice);
	textureCache.Shutdown(nativeDevice);
	materialStates.clear();
	currentScene = nullptr;

	if(lightingBuffer != nullptr) nativeDevice->DestroyBuffer(lightingBuffer);
	lightingBuffer = nullptr;
	if(shadowMatrixBuffer != nullptr) nativeDevice->DestroyBuffer(shadowMatrixBuffer);
	shadowMatrixBuffer = nullptr;
	if(depthStencilTarget != nullptr) nativeDevice->DestroyTexture(depthStencilTarget);
	depthStencilTarget = nullptr;
	depthStencilState = RHI::ResourceState::Undefined;
	if(shadowDepthTarget != nullptr) nativeDevice->DestroyTexture(shadowDepthTarget);
	shadowDepthTarget = nullptr;
	shadowDepthState = RHI::ResourceState::Undefined;
	for(RHI::TextureHandle& texture : defaultMaterialTextures)
	{
		if(texture != nullptr) nativeDevice->DestroyTexture(texture);
		texture = nullptr;
	}
	if(shadowPipeline != nullptr) nativeDevice->DestroyPipeline(shadowPipeline);
	shadowPipeline = nullptr;
	if(pipeline != nullptr) nativeDevice->DestroyPipeline(pipeline);
	pipeline = nullptr;
	if(shadowVertexShader != nullptr) nativeDevice->DestroyShader(shadowVertexShader);
	shadowVertexShader = nullptr;
	if(fragmentShader != nullptr) nativeDevice->DestroyShader(fragmentShader);
	fragmentShader = nullptr;
	if(vertexShader != nullptr) nativeDevice->DestroyShader(vertexShader);
	vertexShader = nullptr;
	device.reset();
}

bool Renderer::Impl::Render(const Scene& scene, const Camera& camera, TextureAsset* readback)
{
	RHI::IDevice* nativeDevice = device.get();
	if(!nativeDevice->BeginFrame()) return false;
	if(!InitializeMesh()) throw std::runtime_error("Mesh pipeline creation failed.");

	if(currentScene != &scene)
	{
		drawData.Shutdown(nativeDevice);
		textureCache.Shutdown(nativeDevice);
		materialStates.clear();
		currentScene = &scene;
	}

	if(!textureCache.Sync(scene, nativeDevice)) throw std::runtime_error("Scene texture upload failed.");
	materialStates.resize(scene.GetMaterialCount());
	UpdateMaterialStates(scene);
	if(!EnsureDepthStencilTarget(nativeDevice) || !EnsureShadowDepthTarget(nativeDevice))
		throw std::runtime_error("Scene depth target creation failed.");

	Private::DrawContext context = {};
	context.clearColor = config.clearColor;
	context.viewProjection = camera.projection * camera.view;
	context.pipeline = pipeline;
	context.materialStates = &materialStates;
	context.depthStencil = depthStencilTarget;
	context.depthStencilState = depthStencilState;
	context.shadowDepth = shadowDepthTarget;
	context.shadowDepthState = shadowDepthState;
	if(!drawData.Prepare(scene, nativeDevice) || !drawData.PrepareDeformations(scene, nativeDevice))
		throw std::runtime_error("Mesh/deformation upload failed.");

	RHI::BufferHandle previousLightingBuffer = lightingBuffer;
	RHI::BufferHandle previousShadowMatrixBuffer = shadowMatrixBuffer;
	lightingBuffer = nullptr;
	shadowMatrixBuffer = nullptr;

	RHI::ICommandList* frameDataCommand = nativeDevice->AcquireCommandList();
	if(frameDataCommand == nullptr)
	{
		lightingBuffer = previousLightingBuffer;
		shadowMatrixBuffer = previousShadowMatrixBuffer;
		throw std::runtime_error("Frame command list acquisition failed.");
	}
	const bool shadowUpdated = UpdateShadowBuffer(scene, nativeDevice, *frameDataCommand);
	const bool lightingUpdated = shadowUpdated &&
		UpdateLightingBuffer(scene, camera, nativeDevice, *frameDataCommand);
	frameDataCommand->Close();
	std::array<RHI::ICommandList*, 1> frameData = { frameDataCommand };
	const bool frameDataSubmitted = nativeDevice->Submit(frameData.data(), 1);
	if(!frameDataSubmitted || !shadowUpdated || !lightingUpdated)
	{
		if(lightingBuffer != nullptr) nativeDevice->DestroyBuffer(lightingBuffer);
		if(shadowMatrixBuffer != nullptr) nativeDevice->DestroyBuffer(shadowMatrixBuffer);
		lightingBuffer = previousLightingBuffer;
		shadowMatrixBuffer = previousShadowMatrixBuffer;
		throw std::runtime_error("Frame data upload/submission failed.");
	}
	if(previousLightingBuffer != nullptr) nativeDevice->DestroyBuffer(previousLightingBuffer);
	if(previousShadowMatrixBuffer != nullptr) nativeDevice->DestroyBuffer(previousShadowMatrixBuffer);

	context.lightingBuffer = lightingBuffer;
	context.shadowMatrixBuffer = shadowMatrixBuffer;
	const DirectionalLight* shadowLight = GetPrimaryDirectionalLight(scene);
	if(shadowPipeline != nullptr && GetPrimaryPointLight(scene) == nullptr &&
		shadowLight != nullptr && shadowLight->castShadow)
	{
		context.shadowPipeline = shadowPipeline;
	}

	if(!drawData.Submit(scene, nativeDevice, context)) throw std::runtime_error("Scene draw submission failed.");
	depthStencilState = depthStencilTarget != nullptr
		? RHI::ResourceState::DepthWrite
		: RHI::ResourceState::Undefined;
	shadowDepthState = shadowDepthTarget != nullptr
		? RHI::ResourceState::ShaderResource
		: RHI::ResourceState::Undefined;
	Private::CaptureFrame(*nativeDevice, readback);
	nativeDevice->Present();
	return true;
}

bool Renderer::Impl::BuildPipelineStates(RHI::IDevice* device)
{
	RHI::TextureHandle backBuffer = device->GetBackBuffer();
	if(backBuffer == nullptr || backBuffer->GetDesc().format == RHI::Format::Unknown ||
		vertexShader == nullptr || fragmentShader == nullptr) return false;

	const RHI::VertexBufferLayout vertexBuffer = {
		0,
		static_cast<uint32_t>(sizeof(Layout::RendererVertex)),
		RHI::VertexStepMode::Vertex
	};
	const std::array<RHI::VertexAttribute, 4> vertexAttributes = {{
		{ 0, 0, RHI::Format::R32G32B32_FLOAT, static_cast<uint32_t>(offsetof(Layout::RendererVertex, px)) },
		{ 1, 0, RHI::Format::R32G32B32_FLOAT, static_cast<uint32_t>(offsetof(Layout::RendererVertex, nx)) },
		{ 2, 0, RHI::Format::R32G32_FLOAT, static_cast<uint32_t>(offsetof(Layout::RendererVertex, u)) },
		{ 3, 0, RHI::Format::R32G32B32A32_FLOAT, static_cast<uint32_t>(offsetof(Layout::RendererVertex, tx)) }
	}};

	RHI::SamplerDesc materialSampler = {};
	materialSampler.minFilter = RHI::SamplerFilter::Linear;
	materialSampler.magFilter = RHI::SamplerFilter::Linear;
	materialSampler.mipFilter = RHI::SamplerFilter::Linear;
	materialSampler.addressU = RHI::SamplerAddressMode::Repeat;
	materialSampler.addressV = RHI::SamplerAddressMode::Repeat;
	materialSampler.addressW = RHI::SamplerAddressMode::Repeat;
	materialSampler.mipLodBias = 0.0f;
	materialSampler.minLod = 0.0f;
	materialSampler.maxLod = 0.0f;

	RHI::SamplerDesc shadowSampler = materialSampler;
	shadowSampler.addressU = RHI::SamplerAddressMode::ClampToEdge;
	shadowSampler.addressV = RHI::SamplerAddressMode::ClampToEdge;
	shadowSampler.addressW = RHI::SamplerAddressMode::ClampToEdge;

	const RHI::ShaderStageFlags vertexAndFragment =
		RHI::ShaderStageFlags::Vertex | RHI::ShaderStageFlags::Fragment;
	const std::array<RHI::ResourceBindingLayout, 12> bindings = {{
		{ RENDERER_BINDING_BASE_COLOR_TEXTURE, RHI::ResourceBindingType::SampledTexture, 1, RHI::ShaderStageFlags::Fragment, {} },
		{ RENDERER_BINDING_LIGHTING_CONSTANTS, RHI::ResourceBindingType::ConstantBuffer, 1, RHI::ShaderStageFlags::Fragment, {} },
		{ RENDERER_BINDING_METALLIC_ROUGHNESS_TEXTURE, RHI::ResourceBindingType::SampledTexture, 1, RHI::ShaderStageFlags::Fragment, {} },
		{ RENDERER_BINDING_NORMAL_TEXTURE, RHI::ResourceBindingType::SampledTexture, 1, RHI::ShaderStageFlags::Fragment, {} },
		{ RENDERER_BINDING_OCCLUSION_TEXTURE, RHI::ResourceBindingType::SampledTexture, 1, RHI::ShaderStageFlags::Fragment, {} },
		{ RENDERER_BINDING_EMISSIVE_TEXTURE, RHI::ResourceBindingType::SampledTexture, 1, RHI::ShaderStageFlags::Fragment, {} },
		{ RENDERER_BINDING_MATERIAL_SAMPLER, RHI::ResourceBindingType::StaticSampler, 1, RHI::ShaderStageFlags::Fragment, materialSampler },
		{ RENDERER_BINDING_SKIN_INFLUENCES, RHI::ResourceBindingType::ReadOnlyStorageBuffer, 1, RHI::ShaderStageFlags::Vertex, {} },
        { RENDERER_BINDING_SKIN_PALETTE, RHI::ResourceBindingType::ReadOnlyStorageBuffer, 1, RHI::ShaderStageFlags::Vertex, {} },
        { RENDERER_BINDING_SHADOW_MATRIX, RHI::ResourceBindingType::ConstantBuffer, 1, RHI::ShaderStageFlags::Vertex, {} },
		{ RENDERER_BINDING_SHADOW_TEXTURE, RHI::ResourceBindingType::SampledTexture, 1, RHI::ShaderStageFlags::Fragment, {} },
		{ RENDERER_BINDING_SHADOW_SAMPLER, RHI::ResourceBindingType::StaticSampler, 1, RHI::ShaderStageFlags::Fragment, shadowSampler }
	}};
	const uint32_t bindingCount = config.enableShadows
		? static_cast<uint32_t>(bindings.size())
		: static_cast<uint32_t>(bindings.size() - 3);

	const RHI::ColorAttachmentDesc colorAttachment = {
		backBuffer->GetDesc().format,
		{ true, RHI::BlendFactor::SourceAlpha, RHI::BlendFactor::OneMinusSourceAlpha, RHI::BlendOp::Add,
			RHI::BlendFactor::One, RHI::BlendFactor::Zero, RHI::BlendOp::Add },
		RHI::ColorWriteMask::All
	};

	RHI::GraphicsPipelineDesc desc = {};
	desc.vertexShader = vertexShader;
	desc.fragmentShader = fragmentShader;
	desc.topology = RHI::PrimitiveTopology::TriangleList;
	desc.vertexBuffers = &vertexBuffer;
	desc.vertexBufferCount = 1;
	desc.vertexAttributes = vertexAttributes.data();
	desc.vertexAttributeCount = static_cast<uint32_t>(vertexAttributes.size());
	desc.raster = { RHI::FillMode::Solid, RHI::CullMode::Back, RHI::FrontFace::CounterClockwise, 0.0f, 0.0f, 0.0f };
	const RHI::Format depthStencilFormat = ToRHIFormat(config.depthStencilFormat);
	desc.depthStencil.format = depthStencilFormat;
	desc.depthStencil.depthTestEnabled = depthStencilFormat != RHI::Format::Unknown;
	desc.depthStencil.depthWriteEnabled = desc.depthStencil.depthTestEnabled;
	desc.depthStencil.depthCompareOp = desc.depthStencil.depthTestEnabled ? RHI::CompareOp::Less : RHI::CompareOp::Always;
	desc.colorAttachments = &colorAttachment;
	desc.colorAttachmentCount = 1;
	desc.layout = {
		bindings.data(),
		bindingCount,
		static_cast<uint32_t>(sizeof(Layout::DrawConstants)),
		vertexAndFragment,
		RENDERER_BINDING_INLINE_CONSTANTS
	};
	pipeline = device->CreateGraphicsPipeline(desc);
	if(pipeline == nullptr) return false;

	if(!config.enableShadows) return true;
	if(shadowVertexShader == nullptr) return false;

	const std::array<RHI::ResourceBindingLayout, 3> shadowBindings = {{
		{ RENDERER_BINDING_SHADOW_MATRIX, RHI::ResourceBindingType::ConstantBuffer, 1, RHI::ShaderStageFlags::Vertex, {} },
		{ RENDERER_BINDING_SKIN_INFLUENCES, RHI::ResourceBindingType::ReadOnlyStorageBuffer, 1, RHI::ShaderStageFlags::Vertex, {} },
		{ RENDERER_BINDING_SKIN_PALETTE, RHI::ResourceBindingType::ReadOnlyStorageBuffer, 1, RHI::ShaderStageFlags::Vertex, {} }
	}};
	RHI::GraphicsPipelineDesc shadowDesc = {};
	shadowDesc.vertexShader = shadowVertexShader;
	shadowDesc.topology = RHI::PrimitiveTopology::TriangleList;
	shadowDesc.vertexBuffers = &vertexBuffer;
	shadowDesc.vertexBufferCount = 1;
	shadowDesc.vertexAttributes = vertexAttributes.data();
	shadowDesc.vertexAttributeCount = static_cast<uint32_t>(vertexAttributes.size());
	shadowDesc.raster = {
		RHI::FillMode::Solid,
		RHI::CullMode::None,
		RHI::FrontFace::CounterClockwise,
		0.0f,
		config.shadowRasterSlopeBias,
		0.0f
	};
	shadowDesc.depthStencil.format = ToRHIFormat(config.shadowFormat);
	shadowDesc.depthStencil.depthTestEnabled = true;
	shadowDesc.depthStencil.depthWriteEnabled = true;
	shadowDesc.depthStencil.depthCompareOp = RHI::CompareOp::Less;
	shadowDesc.layout = {
		shadowBindings.data(),
		static_cast<uint32_t>(shadowBindings.size()),
		static_cast<uint32_t>(sizeof(Layout::DrawConstants)),
		RHI::ShaderStageFlags::Vertex,
		RENDERER_BINDING_INLINE_CONSTANTS
	};
	shadowPipeline = device->CreateGraphicsPipeline(shadowDesc);
	return shadowPipeline != nullptr;
}

bool Renderer::Impl::CreateDefaultMaterialTextures(RHI::IDevice* device)
{
	if(device == nullptr) return false;
	const std::array<std::array<uint8_t, 4>, 3> pixels = {{
		{{ 255, 255, 255, 255 }},
		{{ 128, 128, 255, 255 }},
		{{ 0, 0, 0, 255 }}
	}};

	RHI::TextureDesc desc = {};
	desc.width = 1;
	desc.height = 1;
	desc.depthOrArraySize = 1;
	desc.mipLevels = 1;
	desc.format = RHI::Format::R8G8B8A8_UNORM;
	desc.usage = RHI::TextureUsage::ShaderResource;
	for(RHI::TextureHandle& texture : defaultMaterialTextures)
	{
		texture = device->CreateTexture(desc);
		if(texture == nullptr) return false;
	}

	RHI::ICommandList* commandList = device->AcquireCommandList();
	if(commandList == nullptr) return false;
	std::array<RHI::ResourceBarrierDesc, 3> beforeCopy = {};
	std::array<RHI::ResourceBarrierDesc, 3> barriers = {};
	uint32_t barrierCount = 0;
	bool uploadFailed = false;
	for(uint32_t index = 0; index < defaultMaterialTextures.size(); ++index)
	{
		beforeCopy[index].texture = defaultMaterialTextures[index];
		beforeCopy[index].before = RHI::ResourceState::Undefined;
		beforeCopy[index].after = RHI::ResourceState::CopyDestination;
	}
	commandList->ResourceBarrier(beforeCopy.data(), static_cast<uint32_t>(beforeCopy.size()));
	for(uint32_t index = 0; index < defaultMaterialTextures.size(); ++index)
	{
		if(!device->UpdateTexture(
				*commandList,
				defaultMaterialTextures[index],
				0,
				0,
				pixels[index].data(),
				static_cast<uint32_t>(pixels[index].size()),
				4,
				4))
		{
			uploadFailed = true;
			continue;
		}
		barriers[barrierCount].texture = defaultMaterialTextures[index];
		barriers[barrierCount].before = RHI::ResourceState::CopyDestination;
		barriers[barrierCount].after = RHI::ResourceState::ShaderResource;
		++barrierCount;
	}
	if(barrierCount != 0) commandList->ResourceBarrier(barriers.data(), barrierCount);
	commandList->Close();
	std::array<RHI::ICommandList*, 1> commandLists = { commandList };
	const bool submitted = device->Submit(commandLists.data(), 1);
	return submitted && !uploadFailed;
}

bool Renderer::Impl::EnsureDepthStencilTarget(RHI::IDevice* device)
{
	if(device == nullptr) return false;
	const RHI::Format depthStencilFormat = ToRHIFormat(config.depthStencilFormat);

	if(depthStencilFormat == RHI::Format::Unknown)
	{
		if(depthStencilTarget != nullptr)
		{
			device->DestroyTexture(depthStencilTarget);
			depthStencilTarget = nullptr;
			depthStencilState = RHI::ResourceState::Undefined;
		}
		return true;
	}

	RHI::TextureHandle backBuffer = device->GetBackBuffer();
	if(backBuffer == nullptr || backBuffer->GetDesc().width == 0u || backBuffer->GetDesc().height == 0u)
		return false;

	const bool recreate =
		depthStencilTarget == nullptr ||
		depthStencilTarget->GetDesc().width != backBuffer->GetDesc().width ||
		depthStencilTarget->GetDesc().height != backBuffer->GetDesc().height ||
		depthStencilTarget->GetDesc().format != depthStencilFormat;

	if(!recreate) return true;

	if(depthStencilTarget != nullptr)
	{
		device->DestroyTexture(depthStencilTarget);
		depthStencilTarget = nullptr;
		depthStencilState = RHI::ResourceState::Undefined;
	}

	RHI::TextureDesc depthDesc = {};
	depthDesc.width = backBuffer->GetDesc().width;
	depthDesc.height = backBuffer->GetDesc().height;
	depthDesc.depthOrArraySize = 1;
	depthDesc.mipLevels = 1;
	depthDesc.format = depthStencilFormat;
	depthDesc.usage = RHI::TextureUsage::DepthStencil;
	depthStencilTarget = device->CreateTexture(depthDesc);
	return depthStencilTarget != nullptr;
}

bool Renderer::Impl::EnsureShadowDepthTarget(RHI::IDevice* device)
{
	if(!config.enableShadows) return true;
	if(device == nullptr) return false;
	const RHI::Format shadowFormat = ToRHIFormat(config.shadowFormat);
	if(shadowFormat == RHI::Format::Unknown) return false;

	const uint32_t resolution = config.shadowMap.resolution;
	if(resolution == 0) return false;
	if(shadowDepthTarget != nullptr &&
		shadowDepthTarget->GetDesc().width == resolution &&
		shadowDepthTarget->GetDesc().height == resolution &&
		shadowDepthTarget->GetDesc().format == shadowFormat)
	{
		return true;
	}

	if(shadowDepthTarget != nullptr)
	{
		device->DestroyTexture(shadowDepthTarget);
		shadowDepthTarget = nullptr;
		shadowDepthState = RHI::ResourceState::Undefined;
	}

	RHI::TextureDesc shadowDesc = {};
	shadowDesc.width = resolution;
	shadowDesc.height = resolution;
	shadowDesc.depthOrArraySize = 1;
	shadowDesc.mipLevels = 1;
	shadowDesc.format = shadowFormat;
	shadowDesc.usage = RHI::TextureUsage::DepthStencil | RHI::TextureUsage::ShaderResource;
	shadowDepthTarget = device->CreateTexture(shadowDesc);
	return shadowDepthTarget != nullptr;
}

void Renderer::Impl::UpdateMaterialStates(const Scene& scene)
{
	const uint32_t materialCount = scene.GetMaterialCount();
	for(uint32_t materialIndex = 0; materialIndex < materialCount; ++materialIndex)
	{
		const MaterialDesc& material = scene.GetMaterial(static_cast<MaterialID>(materialIndex));
		Private::SceneMaterialState& materialState = materialStates[materialIndex];
		materialState.textures[ToIndex(MaterialTextureKind::BaseColor)] =
			textureCache.Resolve(material.baseColorTexture);
		materialState.textures[ToIndex(MaterialTextureKind::MetallicRoughness)] =
			textureCache.Resolve(material.metallicRoughnessTexture);
		materialState.textures[ToIndex(MaterialTextureKind::Normal)] =
			textureCache.Resolve(material.normalTexture);
		materialState.textures[ToIndex(MaterialTextureKind::Occlusion)] =
			textureCache.Resolve(material.occlusionTexture);
		materialState.textures[ToIndex(MaterialTextureKind::Emissive)] =
			textureCache.Resolve(material.emissiveTexture);

		uint32_t textureFlags = 0;
		if(materialState.textures[ToIndex(MaterialTextureKind::BaseColor)] != nullptr)
			textureFlags |= RENDERER_TEXTURE_FLAG_BASE_COLOR;
		if(materialState.textures[ToIndex(MaterialTextureKind::MetallicRoughness)] != nullptr)
			textureFlags |= RENDERER_TEXTURE_FLAG_METALLIC_ROUGHNESS;
		if(materialState.textures[ToIndex(MaterialTextureKind::Normal)] != nullptr)
			textureFlags |= RENDERER_TEXTURE_FLAG_NORMAL;
		if(materialState.textures[ToIndex(MaterialTextureKind::Occlusion)] != nullptr)
			textureFlags |= RENDERER_TEXTURE_FLAG_OCCLUSION;
		if(materialState.textures[ToIndex(MaterialTextureKind::Emissive)] != nullptr)
			textureFlags |= RENDERER_TEXTURE_FLAG_EMISSIVE;
		materialState.textureFlags = textureFlags;

		if(materialState.textures[ToIndex(MaterialTextureKind::BaseColor)] == nullptr)
			materialState.textures[ToIndex(MaterialTextureKind::BaseColor)] = defaultMaterialTextures[0];
		if(materialState.textures[ToIndex(MaterialTextureKind::MetallicRoughness)] == nullptr)
			materialState.textures[ToIndex(MaterialTextureKind::MetallicRoughness)] = defaultMaterialTextures[0];
		if(materialState.textures[ToIndex(MaterialTextureKind::Normal)] == nullptr)
			materialState.textures[ToIndex(MaterialTextureKind::Normal)] = defaultMaterialTextures[1];
		if(materialState.textures[ToIndex(MaterialTextureKind::Occlusion)] == nullptr)
			materialState.textures[ToIndex(MaterialTextureKind::Occlusion)] = defaultMaterialTextures[0];
		if(materialState.textures[ToIndex(MaterialTextureKind::Emissive)] == nullptr)
			materialState.textures[ToIndex(MaterialTextureKind::Emissive)] = defaultMaterialTextures[2];
	}
}

bool Renderer::Impl::UpdateLightingBuffer(
	const Scene& scene,
	const Camera& camera,
	RHI::IDevice* device,
	RHI::ICommandList& commandList)
{
	RHI::TextureHandle backBuffer = device->GetBackBuffer();
	if(backBuffer == nullptr) return false;

	if(lightingBuffer == nullptr)
	{
		lightingBuffer = device->CreateBuffer(RHI::BufferDesc{
			static_cast<uint32_t>(sizeof(Layout::RendererLightingConstants)),
			static_cast<uint32_t>(sizeof(Layout::RendererLightingConstants)),
			RHI::BufferUsage::Constant,
			RHI::ResourceState::CopyDestination
		});
	}
	if(lightingBuffer == nullptr) return false;

	const DirectionalLight* light = GetPrimaryDirectionalLight(scene);
	const PointLight* pointLight = GetPrimaryPointLight(scene);
	const Math::float3 lightDirection = light != nullptr
		? light->direction
		: Math::float3(0.0f, 0.0f, 1.0f);
	const Math::float3 lightColor = light != nullptr
		? light->color
		: Math::float3(0.0f, 0.0f, 0.0f);
	const float lightIntensity = light != nullptr ? light->intensity : 0.0f;
	const bool shadowsEnabled = shadowPipeline != nullptr && pointLight == nullptr &&
		light != nullptr && light->castShadow;
	const float shadowStrength = shadowsEnabled ? light->shadowStrength : 0.0f;

	Layout::RendererLightingConstants lighting = {};
	lighting.cameraPosition = Math::float4(
		camera.position.x,
		camera.position.y,
		camera.position.z,
		shadowsEnabled ? shadowStrength : 0.0f);
	lighting.directionalLightDirection = Math::float4(
		lightDirection.x,
		lightDirection.y,
		lightDirection.z,
		shadowsEnabled ? 1.0f : 0.0f);
	lighting.directionalLightColor = Math::float4(lightColor.x, lightColor.y, lightColor.z, lightIntensity);
	lighting.ambientColor = Math::float4(
		config.ambientColor.x * config.environment.diffuseColor.x,
		config.ambientColor.y * config.environment.diffuseColor.y,
		config.ambientColor.z * config.environment.diffuseColor.z,
		config.ambientIntensity * config.environment.diffuseIntensity);
	lighting.shadowParams = Math::float4(
		config.shadowDepthBias,
		config.shadowSlopeBias,
		config.shadowNormalBias,
		static_cast<float>(config.shadowPcfRadius));
	lighting.pbrParams = Math::float4(
		config.pbr.minRoughness,
		config.pbr.ambientSpecularStrength,
		RHI::IsSrgbFormat(backBuffer->GetDesc().format) ? 0.0f : 1.0f,
		0.0f);
	lighting.environmentColor = Math::float4(
		config.environment.specularColor.x,
		config.environment.specularColor.y,
		config.environment.specularColor.z,
		config.environment.specularIntensity);
	if(pointLight != nullptr)
	{
		lighting.pointLightPositionRange = Math::float4(
			pointLight->position.x, pointLight->position.y, pointLight->position.z, pointLight->range);
		lighting.pointLightColorIntensity = Math::float4(
			pointLight->color.x, pointLight->color.y, pointLight->color.z, pointLight->intensity);
	}

	Private::PackSceneLights(scene, lighting);

	if(!device->UpdateBuffer(
			commandList,
			lightingBuffer,
			0,
			&lighting,
			static_cast<uint32_t>(sizeof(lighting))))
		return false;
	const RHI::ResourceBarrierDesc after = {
		lightingBuffer, nullptr,
		RHI::ResourceState::CopyDestination,
		RHI::ResourceState::ConstantBuffer,
		{}
	};
	commandList.ResourceBarrier(&after, 1);
	return true;
}

bool Renderer::Impl::UpdateShadowBuffer(
	const Scene& scene,
	RHI::IDevice* device,
	RHI::ICommandList& commandList)
{
	if(!config.enableShadows) return true;
	if(shadowMatrixBuffer == nullptr)
	{
		shadowMatrixBuffer = device->CreateBuffer(RHI::BufferDesc{
			static_cast<uint32_t>(sizeof(Layout::RendererShadowConstants)),
			static_cast<uint32_t>(sizeof(Layout::RendererShadowConstants)),
			RHI::BufferUsage::Constant,
			RHI::ResourceState::CopyDestination
		});
	}
	if(shadowMatrixBuffer == nullptr) return false;

	const DirectionalLight* light = GetPrimaryDirectionalLight(scene);
	const bool shadowsEnabled = shadowPipeline != nullptr && GetPrimaryPointLight(scene) == nullptr &&
		light != nullptr && light->castShadow;
	const Math::float3 lightDirection = light != nullptr
		? light->direction
		: Math::float3(0.0f, 0.0f, 1.0f);
	ShadowMapDesc shadowMap = config.shadowMap;
	const Math::Bounds3 bounds = shadowsEnabled && config.autoFitShadowMap
		? ComputeShadowBounds(scene)
		: Math::Bounds3{};
	if(bounds.valid)
	{
		shadowMap = FitDirectionalShadowMapToBounds(
			lightDirection, config.shadowMap, bounds.min, bounds.max, config.shadowBoundsPadding);
	}

	Layout::RendererShadowConstants shadow = {};
	shadow.lightViewProjectionMatrix = shadowsEnabled
		? ComputeDirectionalLightViewProj(lightDirection, shadowMap)
		: Math::float4x4::Identity();

	if(!device->UpdateBuffer(
			commandList,
			shadowMatrixBuffer,
			0,
			&shadow,
			static_cast<uint32_t>(sizeof(shadow))))
		return false;
	const RHI::ResourceBarrierDesc after = {
		shadowMatrixBuffer, nullptr,
		RHI::ResourceState::CopyDestination,
		RHI::ResourceState::ConstantBuffer,
		{}
	};
	commandList.ResourceBarrier(&after, 1);
	return true;
}
