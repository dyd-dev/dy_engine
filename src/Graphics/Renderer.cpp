#include "Graphics/Renderer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "Graphics/RenderPath.h"
#include "Graphics/LightingGpu.h"
#include "Graphics/Scene.h"
#include "Graphics/ShadowMath.h"
#include "Graphics/SkinningPass.h"
#include "Math/Math.h"
#include "Platform/Profiler.h"
#include "Platform/Window.h"
#include "RHI/IBuffer.h"
#include "RHI/ICommandList.h"
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
bool Renderer::Initialize(RHI::IDevice* device, const RendererDesc& config)
{
	DY_PROFILE_CPU_ZONE_NAMED("Renderer::Initialize");
	static_assert(Layout::kPushConstantRangeSize == sizeof(Layout::DrawConstants), "Renderer draw constants size mismatch.");

	if(device == nullptr) return false;

	const char* vertexShaderPath = nullptr;
	const char* pixelShaderPath = nullptr;
	const char* shadowVertexShaderPath = nullptr;
	if(!ResolveRendererShaderPaths(config, vertexShaderPath, pixelShaderPath, shadowVertexShaderPath))
	{
		return false;
	}

	m_config = config;
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
	m_toneMapVertexShaderSource.clear();
	m_toneMapPixelShaderSource.clear();
	if(m_config.enableHdrRendering)
	{
		if(m_config.toneMapVertexShaderPath == nullptr || m_config.toneMapPixelShaderPath == nullptr) return false;
		m_toneMapVertexShaderSource = ReadBinaryFile(m_config.toneMapVertexShaderPath);
		m_toneMapPixelShaderSource = ReadBinaryFile(m_config.toneMapPixelShaderPath);
	}
	m_computeSkinningShaderSource.clear();
	const bool computeConfigurationSupported =
		m_config.bindingMode == RendererBindingMode::PerDrawBind
		&& device->SupportsComputeSkinning()
		&& m_config.computeSkinningShaderPath != nullptr;
	const SkinningExecutionDecision skinningDecision = ResolveSkinningExecutionMode(
		m_config.skinningExecutionMode,
		computeConfigurationSupported);
	m_activeSkinningExecutionMode = skinningDecision.active;
	if(m_activeSkinningExecutionMode == SkinningExecutionMode::ComputePreSkin)
	{
		std::ifstream computeShaderFile(m_config.computeSkinningShaderPath, std::ios::binary);
		if(computeShaderFile.good())
		{
			computeShaderFile.close();
			m_computeSkinningShaderSource = ReadBinaryFile(m_config.computeSkinningShaderPath);
		}
		else
		{
			m_activeSkinningExecutionMode = SkinningExecutionMode::VertexShader;
			std::fprintf(stderr, "Compute skinning shader unavailable; falling back to vertex-shader skinning.\n");
		}
	}
	else if(skinningDecision.fellBack)
	{
		std::fprintf(stderr, "Compute skinning unsupported for this renderer configuration; falling back to vertex-shader skinning.\n");
	}

	m_clipYFlip = device->RequiresClipSpaceYFlip();

	BuildRenderPassPlan();
	BuildPipelineStates(device);
	BuildRenderPassPlan();
	m_path = CreateRenderPath(m_config.bindingMode);
	if(m_config.enableProfilerHud)
	{
		m_profilerHud.Initialize(device, m_config.profilerHudStartsExpanded);
	}
	return m_pipeline != nullptr && m_path != nullptr && (!m_config.enableHdrRendering || m_toneMapPipeline != nullptr);
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
	m_config.cameraViewMatrix = view;
	m_config.cameraPosition = camera.eye;
	m_config.cameraForward = Math::NormalizeOr(camera.target - camera.eye, Math::float3(0.0f, 1.0f, 0.0f));
	m_config.cameraUp = camera.up;
	m_config.cameraNearPlane = camera.nearPlane;
	m_config.cameraFarPlane = camera.farPlane;
	m_config.cameraFovYRadians = camera.fovYRadians;
	m_config.cameraAspect = camera.aspect;
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
	DY_PROFILE_CPU_ZONE_NAMED("Renderer::Shutdown");
	if(device == nullptr) return;
	m_profilerHud.Shutdown(device);
	m_hasLastFrameStart = false;
	m_lastCpuRenderMilliseconds = 0.0;

	if(m_path != nullptr) m_path->Shutdown(device);
	m_path.reset();

	m_gpuScene.Shutdown(device);
	m_materialStates.clear();
	m_vertexShaderSource.clear();
	m_pixelShaderSource.clear();
	m_shadowVertexShaderSource.clear();
	m_computeSkinningShaderSource.clear();
	m_toneMapVertexShaderSource.clear();
	m_toneMapPixelShaderSource.clear();
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
	if(m_hdrColorTarget != nullptr)
	{
		device->DestroyTexture(m_hdrColorTarget);
		m_hdrColorTarget = nullptr;
	}
	if(m_shadowDepthTarget != nullptr)
	{
		device->DestroyTexture(m_shadowDepthTarget);
		m_shadowDepthTarget = nullptr;
	}
	m_shadowViewCount = 0u;
	m_shadowAtlasColumns = 1u;
	m_shadowAtlasRows = 1u;
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
	if(m_skinningPipeline != nullptr)
	{
		device->DestroyPipelineState(m_skinningPipeline);
		m_skinningPipeline = nullptr;
	}
	if(m_toneMapPipeline != nullptr)
	{
		device->DestroyPipelineState(m_toneMapPipeline);
		m_toneMapPipeline = nullptr;
	}
	if(m_profilerHudPipeline != nullptr)
	{
		device->DestroyPipelineState(m_profilerHudPipeline);
		m_profilerHudPipeline = nullptr;
	}
	if(m_pipeline != nullptr)
	{
		device->DestroyPipelineState(m_pipeline);
		m_pipeline = nullptr;
	}
	m_activeSkinningExecutionMode = SkinningExecutionMode::VertexShader;
}

void Renderer::Render(const Scene& scene, RHI::IDevice* device)
{
	DY_PROFILE_CPU_ZONE_NAMED("Renderer::Render");
	if(device == nullptr || m_path == nullptr) return;
	const auto renderStart = std::chrono::steady_clock::now();
	double frameMilliseconds = 0.0;
	if(m_hasLastFrameStart)
	{
		frameMilliseconds = std::chrono::duration<double, std::milli>(renderStart - m_lastFrameStart).count();
	}
	m_lastFrameStart = renderStart;
	m_hasLastFrameStart = true;
	if(m_config.enableProfilerHud && Platform::Window::ConsumeKeyPress(Platform::Key::F11))
	{
		m_profilerHud.ToggleExpanded();
	}

	// Backends publish only completed-frame results. Because this lives in the
	// engine renderer, every application using dy_engine gets the same plots.
	RHI::GpuTimestampResult gpuTimestamp = {};
	if(device->TryGetLastGpuTimestamp("Shadow", gpuTimestamp))
	{
		DY_PROFILE_GPU_MILLISECONDS("GPU.Shadow.ms", static_cast<double>(gpuTimestamp.durationNanoseconds) / 1000000.0);
	}
	const bool hasGpuMainTimestamp = device->TryGetLastGpuTimestamp("MainForward", gpuTimestamp);
	const double gpuMainMilliseconds = hasGpuMainTimestamp
		? static_cast<double>(gpuTimestamp.durationNanoseconds) / 1000000.0
		: 0.0;
	if(hasGpuMainTimestamp)
	{
		DY_PROFILE_GPU_MILLISECONDS("GPU.MainForward.ms", gpuMainMilliseconds);
	}
	const RHI::ResourceAllocationCounters resourceCounters = device->GetResourceAllocationCounters();
	DY_PROFILE_RESOURCE_COUNT("GPU.Resources.Buffers.Live", resourceCounters.buffers.live);
	DY_PROFILE_RESOURCE_COUNT("GPU.Resources.Textures.Live", resourceCounters.textures.live);
	DY_PROFILE_RESOURCE_COUNT("GPU.Resources.Pipelines.Live", resourceCounters.pipelines.live);

	// 공유 준비: 텍스처 GPU 레지던시 + 머티리얼 상태(모든 전략 공통).
	m_gpuScene.SyncTextures(scene, device);
	EnsureMaterialStateCapacity(scene.GetMaterialCount());
	UpdateMaterialStates(scene);

	// 전략별 지오메트리/드로우 리소스 준비.
	RenderPathContext context = {};
	context.config = &m_config;
	context.pipeline = m_pipeline;
	context.skinningPipeline = m_skinningPipeline;
	context.skinningExecutionMode = m_activeSkinningExecutionMode;
	context.gpuScene = &m_gpuScene;
	context.materialStates = &m_materialStates;
	EnsureDepthStencilTarget(device);
	EnsureHdrColorTarget(device);
	context.mainColorTarget = m_config.enableHdrRendering ? m_hdrColorTarget : nullptr;
	context.deferSubmit = m_config.enableHdrRendering && m_hdrColorTarget != nullptr && m_toneMapPipeline != nullptr;
	context.depthStencil = m_depthStencilTarget;
	m_path->PrepareResources(scene, device, context);

	// 프레임 상수버퍼는 메인 셰이더가 항상 참조한다(lighting=binding1, shadowMatrix=binding3).
	// 따라서 그림자 비활성이어도 매 프레임 갱신/바인딩한다(그림자 off면 행렬은 Identity).
	UpdateShadowBuffer(scene, device);
	UpdateLightingBuffer(scene, device);
	context.lightingBuffer = m_lightingBuffer;
	context.shadowMatrixBuffer = m_shadowMatrixBuffer;
	if(m_config.enableProfilerHud && m_profilerHudPipeline != nullptr)
	{
		if(RHI::ITexture* backBuffer = device->GetBackBuffer())
		{
			ProfilerHudMetrics metrics = {};
			metrics.frameMilliseconds = frameMilliseconds;
			metrics.fps = frameMilliseconds > 0.001 ? 1000.0 / frameMilliseconds : 0.0;
			metrics.cpuRenderMilliseconds = m_lastCpuRenderMilliseconds;
			metrics.gpuMainMilliseconds = gpuMainMilliseconds;
			metrics.hasGpuMain = hasGpuMainTimestamp;
			metrics.liveBuffers = resourceCounters.buffers.live;
			metrics.createdBuffers = resourceCounters.buffers.created;
			metrics.destroyedBuffers = resourceCounters.buffers.destroyed;
			metrics.liveTextures = resourceCounters.textures.live;
			metrics.createdTextures = resourceCounters.textures.created;
			metrics.destroyedTextures = resourceCounters.textures.destroyed;
			metrics.livePipelines = resourceCounters.pipelines.live;
			metrics.createdPipelines = resourceCounters.pipelines.created;
			metrics.destroyedPipelines = resourceCounters.pipelines.destroyed;
			m_profilerHud.PrepareFrame(device, metrics, backBuffer->GetWidth(), backBuffer->GetHeight(), m_clipYFlip);
			context.profilerHudPipeline = m_profilerHudPipeline;
			context.profilerHud = &m_profilerHud;
		}
	}

	// RenderPath가 메인 패스 전에 깊이 전용 그림자 패스를 기록한다.
	if(m_useExplicitShadowPass)
	{
		EnsureShadowDepthTarget(device);
		if(m_shadowDepthTarget != nullptr && m_shadowPipeline != nullptr)
		{
			context.shadowPipeline = m_shadowPipeline;
			context.shadowDepth = m_shadowDepthTarget;
			context.shadowMapResolution = m_shadowDepthTarget->GetWidth();
			context.shadowViewCount = m_shadowViewCount;
			context.shadowAtlasColumns = m_shadowAtlasColumns;
			context.shadowAtlasRows = m_shadowAtlasRows;
		}
	}

	// The graph schedules CPU recording. Resource transitions and submission
	// remain in the existing RenderPath/RHI implementation.
	m_renderGraph.Reset();
	const auto backBuffer = m_renderGraph.ImportTexture("BackBuffer", device->GetBackBuffer());
	const auto mainColor = context.mainColorTarget != nullptr
		? m_renderGraph.ImportTexture("HdrColor", context.mainColorTarget) : backBuffer;
	const auto depth = m_renderGraph.ImportTexture("Depth", context.depthStencil);
	const auto shadow = m_renderGraph.ImportTexture("Shadow", context.shadowDepth);
	// A logical dependency for the per-entity buffers owned by RenderPath.
	const auto geometry = m_renderGraph.ImportBuffer("SkinnedGeometry", nullptr);
	for(const RenderPassDesc& pass : m_renderPasses)
	{
		if(!pass.enabled) continue;
		if(pass.kind == RenderPassKind::Skinning && pass.work == RenderPassWork::Compute)
		{
			m_renderGraph.AddPass("Skinning").Write(geometry, RGResourceAccess::UnorderedAccess)
				.SetExecute([&](RHI::ICommandList*) { m_path->RecordSkinningPass(scene, device, context); });
		}
		else if(pass.kind == RenderPassKind::Shadow && pass.work == RenderPassWork::Graphics
			&& context.shadowDepth != nullptr && context.shadowPipeline != nullptr)
		{
			m_renderGraph.AddPass("Shadow").Read(geometry, RGResourceAccess::ShaderRead)
				.Write(shadow, RGResourceAccess::DepthWrite)
				.SetExecute([&](RHI::ICommandList*) {
					m_path->RecordShadowPass(scene, device, context);
					context.shadowPassRecorded = true;
				});
		}
		else if(pass.kind == RenderPassKind::MainForward && pass.work == RenderPassWork::Graphics)
		{
			auto& mainPass = m_renderGraph.AddPass("MainForward");
			mainPass.Read(geometry, RGResourceAccess::ShaderRead).Write(mainColor, RGResourceAccess::RenderTarget);
			if(context.shadowDepth != nullptr) mainPass.Read(shadow, RGResourceAccess::ShaderRead);
			if(context.depthStencil != nullptr) mainPass.Write(depth, RGResourceAccess::DepthWrite);
			mainPass.SetExecute([&](RHI::ICommandList*) { m_path->RecordMainPass(scene, device, context); });
		}
	}
	if(context.deferSubmit)
	{
		m_renderGraph.AddPass("ToneMap").Read(mainColor, RGResourceAccess::ShaderRead)
			.Write(backBuffer, RGResourceAccess::RenderTarget)
			.SetExecute([&](RHI::ICommandList*) { RecordToneMapPass(device); });
	}
	if(!m_renderGraph.Compile()) throw std::runtime_error("Renderer RenderGraph contains invalid dependencies.");
	m_renderGraph.Execute(nullptr);
	// Callbacks borrow this Render call's context; do not retain them past it.
	m_renderGraph.Reset();
	m_lastCpuRenderMilliseconds = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - renderStart).count();
}

void Renderer::BuildPipelineStates(RHI::IDevice* device)
{
	DY_PROFILE_CPU_ZONE_NAMED("Renderer::BuildPipelineStates");
	const RHI::GraphicsResourceProfile automaticResourceProfile = [this]()
	{
		switch(m_config.bindingMode)
		{
		case RendererBindingMode::BatchedBind: return RHI::GraphicsResourceProfile::Batched;
		case RendererBindingMode::Bindless: return RHI::GraphicsResourceProfile::Bindless;
		case RendererBindingMode::PerDrawBind: return RHI::GraphicsResourceProfile::PerDrawSkin;
		}
		return RHI::GraphicsResourceProfile::PerDrawSkin;
	}();
	const RHI::GraphicsResourceProfile resourceProfile = m_config.overrideResourceProfile
		? m_config.resourceProfile
		: automaticResourceProfile;
	// 렌더 타깃 포맷은 실제 백버퍼에서 파생한다(단일 진실원). 이래야 PSO 가 실제 타깃과
	// 항상 일치하고, 백엔드별 스왑체인 포맷 불일치(감마 차이)가 생기지 않는다.
	if(RHI::ITexture* backBuffer = device->GetBackBuffer())
	{
		if(backBuffer->GetFormat() != RHI::Format::Unknown)
		{
			m_config.renderTargetFormat = m_config.enableHdrRendering
				? RHI::Format::R16G16B16A16_FLOAT
				: backBuffer->GetFormat();
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
	desc.resourceProfile = resourceProfile;

	m_pipeline = device->CreateGraphicsPipeline(desc);
	if(m_config.enableHdrRendering && !m_toneMapVertexShaderSource.empty() && !m_toneMapPixelShaderSource.empty())
	{
		RHI::GraphicsPipelineDesc toneMapDesc = {};
		toneMapDesc.vertexShader = m_toneMapVertexShaderSource.data();
		toneMapDesc.vertexShaderSize = m_toneMapVertexShaderSource.size();
		toneMapDesc.pixelShader = m_toneMapPixelShaderSource.data();
		toneMapDesc.pixelShaderSize = m_toneMapPixelShaderSource.size();
		toneMapDesc.renderTargetFormat = device->GetBackBuffer() != nullptr
			? device->GetBackBuffer()->GetFormat()
			: RHI::Format::R8G8B8A8_UNORM;
		toneMapDesc.depthStencilFormat = RHI::Format::Unknown;
		toneMapDesc.depthEnable = false;
		toneMapDesc.resourceProfile = RHI::GraphicsResourceProfile::PerDrawSkin;
		m_toneMapPipeline = device->CreateGraphicsPipeline(toneMapDesc);
	}
	if(m_activeSkinningExecutionMode == SkinningExecutionMode::ComputePreSkin)
	{
		RHI::ComputePipelineDesc computeDesc = {};
		computeDesc.computeShader = m_computeSkinningShaderSource.data();
		computeDesc.computeShaderSize = m_computeSkinningShaderSource.size();
		computeDesc.storageBufferCount = 4u;
		computeDesc.inlineConstantSize = 2u * static_cast<uint32_t>(sizeof(uint32_t));
		m_skinningPipeline = device->CreateComputePipeline(computeDesc);
		if(m_skinningPipeline == nullptr)
		{
			m_activeSkinningExecutionMode = SkinningExecutionMode::VertexShader;
			std::fprintf(stderr, "Compute skinning pipeline creation failed; falling back to vertex-shader skinning.\n");
		}
	}
	if(m_config.enableProfilerHud)
	{
		RHI::GraphicsPipelineDesc hudDesc = desc;
		hudDesc.depthEnable = false;
		hudDesc.depthStencilFormat = RHI::Format::Unknown;
		hudDesc.renderTargetFormat = device->GetBackBuffer()->GetFormat();
		m_profilerHudPipeline = device->CreateGraphicsPipeline(hudDesc);
	}

	// 별도의 깊이 전용 PSO(픽셀 셰이더 없음)를 만들어 Graphics에서 그림자 패스를 기록한다.
	m_useExplicitShadowPass =
		IsShadowEnabled() &&
		device->RequiresExplicitShadowPass() &&
		!m_shadowVertexShaderSource.empty();
	if(m_useExplicitShadowPass)
	{
		const RHI::Format shadowFormat = m_config.depthStencilFormat != RHI::Format::Unknown
			? m_config.depthStencilFormat
			: RHI::Format::D32_FLOAT;

		RHI::GraphicsPipelineDesc shadowDesc = {};
		shadowDesc.vertexShader = m_shadowVertexShaderSource.data();
		shadowDesc.vertexShaderSize = m_shadowVertexShaderSource.size();
		shadowDesc.pixelShader = nullptr;
		shadowDesc.pixelShaderSize = 0;
		shadowDesc.renderTargetFormat = RHI::Format::Unknown; // 컬러 출력 없음(깊이 전용)
		shadowDesc.depthStencilFormat = shadowFormat;
		shadowDesc.depthEnable = true;
		shadowDesc.enableBindlessTextures = m_config.enableBindlessTextures;
		shadowDesc.resourceProfile = resourceProfile;
		shadowDesc.depthBias = 1;
		shadowDesc.depthBiasSlope = 1.75f; // Vulkan 그림자 파이프라인과 유사한 슬로프 바이어스

		m_shadowPipeline = device->CreateGraphicsPipeline(shadowDesc);
		if(m_shadowPipeline == nullptr) m_useExplicitShadowPass = false;
	}
}

void Renderer::BuildRenderPassPlan()
{
	m_renderPasses.clear();
	m_renderPasses.push_back(RenderPassDesc{
		RenderPassKind::Skinning,
		RenderPassWork::Compute,
		"Skinning",
		m_activeSkinningExecutionMode == SkinningExecutionMode::ComputePreSkin
	});
	m_renderPasses.push_back(RenderPassDesc{
		RenderPassKind::Shadow,
		RenderPassWork::Graphics,
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

void Renderer::EnsureHdrColorTarget(RHI::IDevice* device)
{
	if(device == nullptr || !m_config.enableHdrRendering)
	{
		if(device != nullptr && m_hdrColorTarget != nullptr)
		{
			device->DestroyTexture(m_hdrColorTarget);
			m_hdrColorTarget = nullptr;
		}
		return;
	}
	RHI::ITexture* backBuffer = device->GetBackBuffer();
	if(backBuffer == nullptr || backBuffer->GetWidth() == 0u || backBuffer->GetHeight() == 0u) return;
	const bool recreate = m_hdrColorTarget == nullptr
		|| m_hdrColorTarget->GetWidth() != backBuffer->GetWidth()
		|| m_hdrColorTarget->GetHeight() != backBuffer->GetHeight()
		|| m_hdrColorTarget->GetFormat() != RHI::Format::R16G16B16A16_FLOAT;
	if(!recreate) return;
	if(m_hdrColorTarget != nullptr) device->DestroyTexture(m_hdrColorTarget);
	RHI::TextureDesc hdrDesc = {};
	hdrDesc.width = backBuffer->GetWidth();
	hdrDesc.height = backBuffer->GetHeight();
	hdrDesc.depthOrArraySize = 1u;
	hdrDesc.mipLevels = 1u;
	hdrDesc.format = RHI::Format::R16G16B16A16_FLOAT;
	hdrDesc.usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::ShaderResource;
	m_hdrColorTarget = device->CreateTexture(hdrDesc);
}

void Renderer::RecordToneMapPass(RHI::IDevice* device)
{
	if(device == nullptr || m_hdrColorTarget == nullptr || m_toneMapPipeline == nullptr) return;
	RHI::ICommandList* commandList = device->AcquireCommandList();
	RHI::ITexture* backBuffer = device->GetBackBuffer();
	if(commandList == nullptr || backBuffer == nullptr) return;
	commandList->SetRenderTargets(1u, &backBuffer, nullptr);
	commandList->ClearColor(backBuffer, 0.0f, 0.0f, 0.0f, 1.0f);
	commandList->BindGraphicsPipeline(m_toneMapPipeline);
	commandList->BindTexture(Layout::kBaseColorTextureBinding, m_hdrColorTarget);
	Layout::DrawConstants constants = {};
	constants.baseColor = Math::float4(
		std::max(m_config.exposure, 0.0f),
		RHI::IsSrgbFormat(backBuffer->GetFormat()) ? 0.0f : 1.0f,
		0.0f,
		0.0f);
	commandList->SetInlineConstants(sizeof(constants), &constants);
	commandList->DrawInstanced(3u, 1u, 0u, 0u);
	if(m_config.enableProfilerHud) m_profilerHud.Record(commandList, m_profilerHudPipeline, m_lightingBuffer, m_shadowMatrixBuffer);
	commandList->Close();
	RHI::ICommandList* commandLists[] = { commandList };
	device->Submit(commandLists, 1u);
}

void Renderer::EnsureShadowDepthTarget(RHI::IDevice* device)
{
	if(device == nullptr || !m_useExplicitShadowPass) return;

	const uint32_t resolution = m_config.shadowMap.resolution > 0u ? m_config.shadowMap.resolution : 2048u;
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
	DY_PROFILE_CPU_ZONE_NAMED("Renderer::UpdateMaterialStates");
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
	DY_PROFILE_CPU_ZONE_NAMED("Renderer::UpdateLightingBuffer");
	if(m_lightingBuffer == nullptr)
	{
		m_lightingBuffer = device->CreateBuffer(RHI::BufferDesc{
			static_cast<uint32_t>(sizeof(Layout::RendererLightingConstants)),
			static_cast<uint32_t>(sizeof(Layout::RendererLightingConstants)),
			RHI::BufferUsage::Constant
		});
	}
	if(m_lightingBuffer == nullptr) return;

	RendererDesc lightingConfig = m_config;
	lightingConfig.enableShadows = IsShadowEnabled();
	Layout::RendererLightingConstants lighting = BuildRendererLightingConstants(scene, lightingConfig);

	void* data = m_lightingBuffer->Map(0);
	if(data != nullptr)
	{
		std::memcpy(data, &lighting, sizeof(lighting));
		m_lightingBuffer->Unmap();
	}
}

void Renderer::UpdateShadowBuffer(const Scene& scene, RHI::IDevice* device)
{
	DY_PROFILE_CPU_ZONE_NAMED("Renderer::UpdateShadowBuffer");
	if(m_shadowMatrixBuffer == nullptr)
	{
		m_shadowMatrixBuffer = device->CreateBuffer(RHI::BufferDesc{
			static_cast<uint32_t>(sizeof(Layout::RendererShadowConstants)),
			static_cast<uint32_t>(sizeof(Layout::RendererShadowConstants)),
			RHI::BufferUsage::Constant
		});
	}
	if(m_shadowMatrixBuffer == nullptr) return;

	Layout::RendererShadowConstants shadow = {};
	for(Math::float4x4& matrix : shadow.lightViewProjectionMatrices) matrix = Math::float4x4::Identity();
	shadow.cameraViewMatrix = m_config.cameraViewMatrix;
	ShadowLightSelection shadowSelection;
	[[maybe_unused]] const Layout::RendererLightingConstants lighting = BuildRendererLightingConstants(
		scene, m_config, IsShadowEnabled(), &shadowSelection);
	float shadowNearPlane = std::max(m_config.shadowMap.nearPlane, 0.0001f);
	if(IsShadowEnabled() && shadowSelection.type == ShadowLightType::Directional)
	{
		Math::float3 lightDirection = m_config.directionalLightDirection;
		if(!scene.DirectionalLights().empty())
		{
			lightDirection = scene.GetDirectionalLight(shadowSelection.sceneIndex).direction;
		}
		CameraFrustumDesc camera;
		camera.position = m_config.cameraPosition;
		camera.forward = m_config.cameraForward;
		camera.up = m_config.cameraUp;
		camera.nearPlane = m_config.cameraNearPlane;
		camera.farPlane = m_config.cameraFarPlane;
		camera.fovYRadians = m_config.cameraFovYRadians;
		camera.aspect = m_config.cameraAspect;
		const DirectionalCascadeData cascades = ComputeDirectionalCascades(
			camera,
			lightDirection,
			m_config.shadowMap,
			m_config.shadowCascadeCount,
			m_config.shadowCascadeSplitLambda,
			m_config.shadowBoundsPadding);
		for(uint32_t index = 0u; index < cascades.count; ++index)
		{
			shadow.lightViewProjectionMatrices[index] = cascades.viewProjections[index];
		}
		shadow.cascadeSplits = Math::float4(cascades.splits[0], cascades.splits[1], cascades.splits[2], cascades.splits[3]);
		shadow.shadowInfo = Math::float4(static_cast<float>(ShadowLightType::Directional), static_cast<float>(cascades.count), 2.0f, 2.0f);
	}
	else if(IsShadowEnabled() && shadowSelection.type == ShadowLightType::Spot)
	{
		const SpotLight& spot = scene.GetSpotLight(shadowSelection.sceneIndex);
		ShadowMapDesc shadowMap = m_config.shadowMap;
		shadowMap.farPlane = std::max(spot.range, shadowMap.nearPlane + 0.1f);
		shadowMap.spotFovYRadians = std::clamp(spot.outerConeRadians * 2.0f, 0.1f, 3.0f);
		shadow.lightViewProjectionMatrices[0] = ComputeSpotLightViewProj(spot.position, spot.direction, shadowMap);
		shadow.shadowInfo = Math::float4(static_cast<float>(ShadowLightType::Spot), 1.0f, 1.0f, 1.0f);
		shadowNearPlane = std::max(shadowMap.nearPlane, 0.0001f);
	}
	else if(IsShadowEnabled() && shadowSelection.type == ShadowLightType::Point)
	{
		const PointLight& point = scene.GetPointLight(shadowSelection.sceneIndex);
		const float farPlane = std::max(point.range, shadowNearPlane + 0.1f);
		shadow.lightViewProjectionMatrices = ComputePointLightViewProjections(point.position, shadowNearPlane, farPlane);
		shadow.shadowInfo = Math::float4(static_cast<float>(ShadowLightType::Point), 6.0f, 3.0f, 2.0f);
	}
	else shadow.shadowInfo = Math::float4(static_cast<float>(ShadowLightType::None), 0.0f, 1.0f, 1.0f);
	if(m_useExplicitShadowPass && !device->SupportsShadowAtlas() && IsShadowEnabled())
	{
		const Math::Bounds3 bounds = m_config.autoFitShadowMap ? ComputeShadowBounds(scene) : Math::Bounds3{};
		if(shadowSelection.type == ShadowLightType::Directional)
		{
			Math::float3 direction = m_config.directionalLightDirection;
			if(!scene.DirectionalLights().empty()) direction = scene.GetDirectionalLight(shadowSelection.sceneIndex).direction;
			ShadowMapDesc map = m_config.shadowMap;
			if(bounds.valid) map = FitDirectionalShadowMapToBounds(direction, map, bounds.min, bounds.max, m_config.shadowBoundsPadding);
			shadow.lightViewProjectionMatrices[0] = ComputeDirectionalLightViewProj(direction, map);
		}
		else if(shadowSelection.type == ShadowLightType::Point)
		{
			const PointLight& point = scene.GetPointLight(shadowSelection.sceneIndex);
			ShadowMapDesc map = m_config.shadowMap;
			map.farPlane = std::max(map.farPlane, point.range);
			Math::float3 direction = Math::NormalizeOr(point.direction, Math::float3(0.0f, 0.0f, -1.0f));
			if(bounds.valid) direction = Math::NormalizeOr(bounds.Center() - point.position, direction);
			shadow.lightViewProjectionMatrices[0] = ComputeSpotLightViewProj(point.position, direction, map);
		}
		shadow.shadowInfo.y = 1.0f;
		shadow.shadowInfo.z = 1.0f;
		shadow.shadowInfo.w = 1.0f;
	}
	m_shadowViewCount = std::clamp(static_cast<uint32_t>(std::max(shadow.shadowInfo.y, 0.0f) + 0.5f), 0u, Layout::kMaxShadowViews);
	m_shadowAtlasColumns = std::clamp(static_cast<uint32_t>(std::max(shadow.shadowInfo.z, 1.0f) + 0.5f), 1u, Layout::kMaxShadowViews);
	m_shadowAtlasRows = std::clamp(static_cast<uint32_t>(std::max(shadow.shadowInfo.w, 1.0f) + 0.5f), 1u, Layout::kMaxShadowViews);
	const uint64_t atlasCapacity = static_cast<uint64_t>(m_shadowAtlasColumns) * m_shadowAtlasRows;
	if(m_shadowViewCount > atlasCapacity) m_shadowViewCount = static_cast<uint32_t>(atlasCapacity);
	shadow.pcssParams = Math::float4(
		std::max(m_config.shadowLightRadius, 0.0f),
		std::max(m_config.shadowBlockerSearchRadius, 0.0f),
		std::max(m_config.shadowMaxFilterRadius, 1.0f),
		shadowNearPlane);

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
