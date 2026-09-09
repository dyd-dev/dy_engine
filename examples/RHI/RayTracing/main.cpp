// 미구현 RayTracing 기능의 공개 RHI 사용안. 재설계 중인 API를 사용하며 현재 빌드 대상이 아니다.
// 추가 수명 API: DiscardCommandList(미제출 목록 반환), WaitIdle(GPU 사용 종료까지 대기).
// WaitIdle은 device loss를 포함해 더 이상 자원에 접근하지 않는 상태에서 반환하는 noexcept 계약이다.
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "Platform/Window.h"
#include "Math/Math.h"
#include "RHI/Binding.h"
#include "RHI/Buffer.h"
#include "RHI/ICommandList.h"
#include "RHI/IDevice.h"
#include "RHI/Pipeline.h"
#include "RHI/RayTracing.h"
#include "RHI/Rendering.h"
#include "RHI/ResourceSet.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"

// RayTracingPipeline capability가 있는 device의 staged ray tracing 사용안이다.
// ray query/compute 방식과 구분하며, Metal도 이 pipeline 모델을 지원한다고 가정하지 않는다.
// shader ABI: AS=0, storage image=1, inline camera=2; payload=float4, hit attribute=float2.
// raygen은 camera에서 primary ray를 쏘고 모든 output pixel을 쓴다.
// payload.rgb는 색, payload.w는 남은 반사 횟수(초깃값 1)다. closest-hit은
// primitive 0/1인 바닥에서 남은 횟수를 줄여 반사한다. 나머지는 불투명 삼각형이다.
// miss는 배경색을 쓴다. fullscreen shader는 vertex ID로 삼각형을 만들고 texture를 표시한다.

namespace
{
	using namespace dy;

	struct Vertex
	{
		float position[3];
	};

	constexpr std::array<Vertex, 7> Vertices = {{
		{{-2.0f, -1.0f, -2.0f}},
		{{ 2.0f, -1.0f, -2.0f}},
		{{ 2.0f, -1.0f,  2.0f}},
		{{-2.0f, -1.0f,  2.0f}},
		{{-0.7f,  0.0f,  0.0f}},
		{{ 0.7f,  0.0f,  0.0f}},
		{{ 0.0f,  1.0f,  0.0f}}
	}};
	constexpr std::array<uint32_t, 9> Indices = {{
		0, 2, 1, 0, 3, 2, 4, 5, 6
	}};

	struct RayConstants
	{
		Math::float4 origin;
		Math::float4 forward;
		Math::float4 right;
		Math::float4 up;
	};

	struct ShaderAsset
	{
		const char* path;
		const char* entryPoint;
		RHI::ShaderStage stage;
	};

	[[nodiscard]] std::vector<std::byte> ReadBinary(const std::filesystem::path& path)
	{
		std::ifstream stream(path, std::ios::binary | std::ios::ate);
		if(!stream) throw std::runtime_error("셰이더 바이너리를 열 수 없습니다: " + path.string());
		const std::streamsize size = stream.tellg();
		if(size <= 0) throw std::runtime_error("셰이더 바이너리가 비어 있습니다: " + path.string());
		stream.seekg(0, std::ios::beg);
		std::vector<std::byte> result(static_cast<std::size_t>(size));
		if(!stream.read(reinterpret_cast<char*>(result.data()), size))
			throw std::runtime_error("셰이더 바이너리를 읽을 수 없습니다: " + path.string());
		return result;
	}

	void Require(bool condition, const char* message)
	{
		if(!condition) throw std::runtime_error(message);
	}

	// 제안 수명 계약: Submit은 닫힌 목록을 소비한다. 미제출 목록은 명시적으로 반환한다.
	class CommandList
	{
	public:
		explicit CommandList(RHI::IDevice& device) : m_device(device), m_list(device.AcquireCommandList())
		{
			Require(m_list != nullptr, "Failed to acquire command list");
		}
		~CommandList() { if(m_list != nullptr) m_device.DiscardCommandList(m_list); }
		CommandList(const CommandList&) = delete;
		CommandList& operator=(const CommandList&) = delete;
		RHI::ICommandList* operator->() const { return m_list; }
		RHI::ICommandList& operator*() const { return *m_list; }
		void Submit()
		{
			m_list->Close();
			RHI::ICommandList* lists[] = { m_list };
			m_list = nullptr;
			Require(m_device.Submit(lists, 1), "Command submission failed");
		}
	private:
		RHI::IDevice& m_device;
		RHI::ICommandList* m_list;
	};

	[[nodiscard]] RHI::ShaderHandle CreateShader(RHI::IDevice& device, const ShaderAsset& asset)
	{
		const std::vector<std::byte> binary = ReadBinary(asset.path);
		RHI::ShaderHandle shader = device.CreateShader({
			asset.stage, asset.entryPoint, binary.data(), binary.size()
		});
		Require(shader != nullptr, "셰이더 생성에 실패했습니다.");
		return shader;
	}

	[[nodiscard]] RHI::SamplerDesc LinearSampler()
	{
		RHI::SamplerDesc sampler = {};
		sampler.minFilter = RHI::SamplerFilter::Linear;
		sampler.magFilter = RHI::SamplerFilter::Linear;
		sampler.mipFilter = RHI::SamplerFilter::Linear;
		sampler.addressU = RHI::SamplerAddressMode::ClampToEdge;
		sampler.addressV = RHI::SamplerAddressMode::ClampToEdge;
		sampler.addressW = RHI::SamplerAddressMode::ClampToEdge;
		sampler.mipLodBias = 0.0f;
		sampler.minLod = 0.0f;
		sampler.maxLod = 0.0f;
		return sampler;
	}

	struct Resources
	{
		RHI::BufferHandle vertexBuffer = nullptr;
		RHI::BufferHandle indexBuffer = nullptr;
		RHI::BufferHandle bottomScratch = nullptr;
		RHI::BufferHandle topScratch = nullptr;
		RHI::ShaderTableHandle shaderTable = nullptr;
		RHI::AccelerationStructureHandle bottomLevel = nullptr;
		RHI::AccelerationStructureHandle topLevel = nullptr;
		RHI::TextureHandle reflection = nullptr;
		RHI::ShaderHandle rayGeneration = nullptr;
		RHI::ShaderHandle miss = nullptr;
		RHI::ShaderHandle closestHit = nullptr;
		RHI::ShaderHandle fullscreenVertex = nullptr;
		RHI::ShaderHandle presentFragment = nullptr;
		RHI::PipelineHandle rayPipeline = nullptr;
		RHI::PipelineHandle presentPipeline = nullptr;
		RHI::ResourceSetHandle rayResources = nullptr;
		RHI::ResourceSetHandle presentResources = nullptr;
		RHI::ResourceState reflectionState = RHI::ResourceState::Undefined;
		uint32_t width = 0;
		uint32_t height = 0;

		void DestroyOutput(RHI::IDevice& device)
		{
			device.WaitIdle();
			if(presentResources != nullptr) device.DestroyResourceSet(presentResources);
			if(rayResources != nullptr) device.DestroyResourceSet(rayResources);
			if(reflection != nullptr) device.DestroyTexture(reflection);
			presentResources = nullptr;
			rayResources = nullptr;
			reflection = nullptr;
			reflectionState = RHI::ResourceState::Undefined;
			width = 0;
			height = 0;
		}

		void Destroy(RHI::IDevice& device)
		{
			DestroyOutput(device);
			if(shaderTable != nullptr) device.DestroyShaderTable(shaderTable);
			if(presentPipeline != nullptr) device.DestroyPipeline(presentPipeline);
			if(rayPipeline != nullptr) device.DestroyPipeline(rayPipeline);
			if(presentFragment != nullptr) device.DestroyShader(presentFragment);
			if(fullscreenVertex != nullptr) device.DestroyShader(fullscreenVertex);
			if(closestHit != nullptr) device.DestroyShader(closestHit);
			if(miss != nullptr) device.DestroyShader(miss);
			if(rayGeneration != nullptr) device.DestroyShader(rayGeneration);
			if(topLevel != nullptr) device.DestroyAccelerationStructure(topLevel);
			if(bottomLevel != nullptr) device.DestroyAccelerationStructure(bottomLevel);
			if(topScratch != nullptr) device.DestroyBuffer(topScratch);
			if(bottomScratch != nullptr) device.DestroyBuffer(bottomScratch);
			if(indexBuffer != nullptr) device.DestroyBuffer(indexBuffer);
			if(vertexBuffer != nullptr) device.DestroyBuffer(vertexBuffer);
			*this = {};
		}
	};

	void CreateGeometry(RHI::IDevice& device, Resources& resources)
	{
		resources.vertexBuffer = device.CreateBuffer({
			static_cast<uint32_t>(sizeof(Vertices)),
			static_cast<uint32_t>(sizeof(Vertex)),
			RHI::BufferUsage::AccelerationStructureBuildInput | RHI::BufferUsage::CopyDestination,
			RHI::ResourceState::CopyDestination
		});
		resources.indexBuffer = device.CreateBuffer({
			static_cast<uint32_t>(sizeof(Indices)),
			static_cast<uint32_t>(sizeof(uint32_t)),
			RHI::BufferUsage::AccelerationStructureBuildInput | RHI::BufferUsage::CopyDestination,
			RHI::ResourceState::CopyDestination
		});
		Require(resources.vertexBuffer != nullptr && resources.indexBuffer != nullptr,
			"geometry buffer 생성에 실패했습니다.");

		CommandList upload(device);
		Require(device.UpdateBuffer(
			*upload, resources.vertexBuffer, 0, Vertices.data(), static_cast<uint32_t>(sizeof(Vertices))),
			"vertex upload에 실패했습니다.");
		Require(device.UpdateBuffer(
			*upload, resources.indexBuffer, 0, Indices.data(), static_cast<uint32_t>(sizeof(Indices))),
			"index upload에 실패했습니다.");
		const std::array<RHI::ResourceBarrierDesc, 2> afterUpload = {{
			{ resources.vertexBuffer, nullptr, RHI::ResourceState::CopyDestination,
				RHI::ResourceState::AccelerationStructureBuildInput, {} },
			{ resources.indexBuffer, nullptr, RHI::ResourceState::CopyDestination,
				RHI::ResourceState::AccelerationStructureBuildInput, {} }
		}};
		upload->ResourceBarrier(afterUpload.data(), static_cast<uint32_t>(afterUpload.size()));
		upload.Submit();

		RHI::BottomLevelAccelerationStructureDesc bottomLevel = {};
		bottomLevel.vertexBuffer = resources.vertexBuffer;
		bottomLevel.vertexCount = static_cast<uint32_t>(Vertices.size());
		bottomLevel.vertexStride = static_cast<uint32_t>(sizeof(Vertex));
		bottomLevel.vertexFormat = RHI::Format::R32G32B32_FLOAT;
		bottomLevel.indexBuffer = resources.indexBuffer;
		bottomLevel.indexCount = static_cast<uint32_t>(Indices.size());
		bottomLevel.indexFormat = RHI::Format::R32_UINT;
		bottomLevel.opaque = true;
		bottomLevel.buildFlags = RHI::AccelerationStructureBuildFlags::PreferFastTrace;
		resources.bottomLevel = device.CreateBottomLevelAccelerationStructure(bottomLevel);
		Require(resources.bottomLevel != nullptr, "bottom-level acceleration structure 생성에 실패했습니다.");

		RHI::AccelerationStructureInstance instance = {};
		instance.bottomLevel = resources.bottomLevel;
		instance.transform = Math::float4x4::Identity();
		instance.mask = 0xff;
		instance.instanceId = 0;
		instance.hitGroupOffset = 0;
		instance.flags = RHI::AccelerationStructureInstanceFlags::TriangleCullDisable;
		resources.topLevel = device.CreateTopLevelAccelerationStructure({ &instance, 1,
			RHI::AccelerationStructureBuildFlags::PreferFastTrace });
		Require(resources.topLevel != nullptr, "top-level acceleration structure 생성에 실패했습니다.");

		// scratch 크기/정렬은 device의 요구사항이다. 각 build가 사용할 자원을 호출자가 지정한다.
		auto createScratch = [&](RHI::AccelerationStructureHandle structure)
		{
			const uint64_t size = device.GetAccelerationStructureBuildScratchSize(structure);
			Require(size != 0 && size <= std::numeric_limits<uint32_t>::max(), "Invalid AS scratch size");
			return device.CreateBuffer({ static_cast<uint32_t>(size), 0,
				RHI::BufferUsage::AccelerationStructureScratch, RHI::ResourceState::UnorderedAccess });
		};
		resources.bottomScratch = createScratch(resources.bottomLevel);
		resources.topScratch = createScratch(resources.topLevel);
		Require(resources.bottomScratch != nullptr && resources.topScratch != nullptr, "AS scratch allocation failed");
		CommandList build(device);
		build->BuildAccelerationStructure(resources.bottomLevel, resources.bottomScratch, 0);
		build->AccelerationStructureBarrier({ resources.bottomLevel,
			RHI::AccelerationStructureAccess::BuildWrite, RHI::AccelerationStructureAccess::BuildRead });
		build->BuildAccelerationStructure(resources.topLevel, resources.topScratch, 0);
		build->AccelerationStructureBarrier({ resources.bottomLevel,
			RHI::AccelerationStructureAccess::BuildRead, RHI::AccelerationStructureAccess::TraceRead });
		build->AccelerationStructureBarrier({ resources.topLevel,
			RHI::AccelerationStructureAccess::BuildWrite, RHI::AccelerationStructureAccess::TraceRead });
		build.Submit();
	}

	void CreatePipelines(RHI::IDevice& device, RHI::Format backBufferFormat, Resources& resources,
		const std::array<ShaderAsset, 5>& shaders)
	{
		resources.rayGeneration = CreateShader(device, shaders[0]);
		resources.miss = CreateShader(device, shaders[1]);
		resources.closestHit = CreateShader(device, shaders[2]);
		resources.fullscreenVertex = CreateShader(device, shaders[3]);
		resources.presentFragment = CreateShader(device, shaders[4]);

		const std::array<RHI::ResourceBindingLayout, 2> rayBindings = {{
			{ 0, RHI::ResourceBindingType::AccelerationStructure, 1,
				RHI::ShaderStageFlags::RayTracing, {} },
			{ 1, RHI::ResourceBindingType::StorageTexture, 1,
				RHI::ShaderStageFlags::RayTracing, {} }
		}};
		RHI::RayTracingPipelineDesc rayPipeline = {};
		rayPipeline.rayGenerationShader = resources.rayGeneration;
		rayPipeline.missShader = resources.miss;
		rayPipeline.closestHitShader = resources.closestHit;
		rayPipeline.maxRecursionDepth = 2; // primary + 한 번의 reflection
		rayPipeline.maxPayloadSize = sizeof(Math::float4);
		rayPipeline.maxAttributeSize = sizeof(Math::float2);
		rayPipeline.layout = {
			rayBindings.data(), static_cast<uint32_t>(rayBindings.size()),
			static_cast<uint32_t>(sizeof(RayConstants)), RHI::ShaderStageFlags::RayTracing, 2
		};
		resources.rayPipeline = device.CreateRayTracingPipeline(rayPipeline);
		Require(resources.rayPipeline != nullptr, "ray tracing pipeline 생성에 실패했습니다.");

		// shader 선택과 record 순서는 호출자가 정한다. backend는 요구 alignment/handle을 번역한다.
		RHI::ShaderTableDesc table = {};
		table.pipeline = resources.rayPipeline;
		table.rayGenerationShader = resources.rayGeneration;
		table.missShaders = &resources.miss;
		table.missShaderCount = 1;
		const RHI::RayTracingHitGroup hitGroup = { resources.closestHit, nullptr, nullptr };
		table.hitGroups = &hitGroup;
		table.hitGroupCount = 1;
		resources.shaderTable = device.CreateShaderTable(table);
		Require(resources.shaderTable != nullptr, "Shader table creation failed");

		const std::array<RHI::ResourceBindingLayout, 2> presentBindings = {{
			{ 0, RHI::ResourceBindingType::SampledTexture, 1, RHI::ShaderStageFlags::Fragment, {} },
			{ 1, RHI::ResourceBindingType::StaticSampler, 1, RHI::ShaderStageFlags::Fragment, LinearSampler() }
		}};
		const RHI::ColorAttachmentDesc colorAttachment = {
			backBufferFormat, {}, RHI::ColorWriteMask::All
		};
		RHI::GraphicsPipelineDesc present = {};
		present.vertexShader = resources.fullscreenVertex;
		present.fragmentShader = resources.presentFragment;
		present.topology = RHI::PrimitiveTopology::TriangleList;
		present.raster = {
			RHI::FillMode::Solid, RHI::CullMode::None, RHI::FrontFace::CounterClockwise,
			0.0f, 0.0f, 0.0f
		};
		present.depthStencil.depthTestEnabled = false;
		present.depthStencil.depthWriteEnabled = false;
		present.colorAttachments = &colorAttachment;
		present.colorAttachmentCount = 1;
		present.layout = {
			presentBindings.data(), static_cast<uint32_t>(presentBindings.size()),
			0, RHI::ShaderStageFlags::None, 0
		};
		resources.presentPipeline = device.CreateGraphicsPipeline(present);
		Require(resources.presentPipeline != nullptr, "present pipeline 생성에 실패했습니다.");
	}

	void CreateOutput(RHI::IDevice& device, uint32_t width, uint32_t height, Resources& resources)
	{
		Require(width != 0 && height != 0, "Backbuffer extent must be positive");
		if(resources.width == width && resources.height == height) return;
		resources.DestroyOutput(device);
		resources.reflection = device.CreateTexture({
			width, height, 1, 1, RHI::Format::R8G8B8A8_UNORM,
			RHI::TextureUsage::Storage | RHI::TextureUsage::ShaderResource
		});
		Require(resources.reflection != nullptr, "reflection texture 생성에 실패했습니다.");

		std::array<RHI::ResourceBinding, 2> rayBindings = {};
		rayBindings[0].binding = 0;
		rayBindings[0].accelerationStructure = resources.topLevel;
		rayBindings[1].binding = 1;
		rayBindings[1].texture = resources.reflection;
		resources.rayResources = device.CreateResourceSet({
			resources.rayPipeline, rayBindings.data(), static_cast<uint32_t>(rayBindings.size())
		});
		Require(resources.rayResources != nullptr, "ray tracing resource set 생성에 실패했습니다.");

		RHI::ResourceBinding presentBinding = {};
		presentBinding.binding = 0;
		presentBinding.texture = resources.reflection;
		resources.presentResources = device.CreateResourceSet({
			resources.presentPipeline, &presentBinding, 1
		});
		Require(resources.presentResources != nullptr, "present resource set 생성에 실패했습니다.");
		resources.width = width;
		resources.height = height;
	}

	void RenderFrame(RHI::IDevice& device, Resources& resources)
	{
		RHI::TextureHandle backBuffer = device.GetBackBuffer();
		Require(backBuffer != nullptr, "backbuffer를 얻지 못했습니다.");
		CreateOutput(device, backBuffer->GetDesc().width, backBuffer->GetDesc().height, resources);

		CommandList commandList(device);
		const RHI::ResourceBarrierDesc beforeTrace = {
			nullptr, resources.reflection,
			resources.reflectionState, RHI::ResourceState::UnorderedAccess, {}
		};
		commandList->ResourceBarrier(&beforeTrace, 1);
		commandList->BindRayTracingPipeline(resources.rayPipeline);
		commandList->BindResourceSet(resources.rayResources);
		// 카메라 방향과 반사 정책은 shader/호출자 소유이며 backend는 결정하지 않는다.
		const float aspect = static_cast<float>(resources.width) / resources.height;
		const RayConstants camera = {
			Math::float4(0.0f, 1.0f, 4.0f, 1.0f),
			Math::float4(0.0f, -0.242536f, -0.970143f, 0.0f),
			Math::float4(aspect * 0.577350f, 0.0f, 0.0f, 0.0f),
			Math::float4(0.0f, 0.560112f, -0.140028f, 0.0f)
		};
		commandList->SetInlineConstants(0, static_cast<uint32_t>(sizeof(camera)), &camera);
		commandList->TraceRays(resources.shaderTable, resources.width, resources.height, 1);

		const std::array<RHI::ResourceBarrierDesc, 2> beforePresentPass = {{
			{ nullptr, resources.reflection, RHI::ResourceState::UnorderedAccess,
				RHI::ResourceState::ShaderResource, {} },
			{ nullptr, backBuffer, RHI::ResourceState::Present,
				RHI::ResourceState::RenderTarget, {} }
		}};
		commandList->ResourceBarrier(beforePresentPass.data(),
			static_cast<uint32_t>(beforePresentPass.size()));

		RHI::ColorAttachment color = {};
		color.texture = backBuffer;
		color.loadOp = RHI::LoadOp::Discard;
		color.storeOp = RHI::StoreOp::Store;
		commandList->BeginRendering({ &color, 1, nullptr });
		commandList->BindGraphicsPipeline(resources.presentPipeline);
		commandList->BindResourceSet(resources.presentResources);
		commandList->SetViewport({
			0.0f, 0.0f, static_cast<float>(resources.width),
			static_cast<float>(resources.height), 0.0f, 1.0f
		});
		commandList->SetScissor({ 0, 0, resources.width, resources.height });
		commandList->DrawInstanced(3, 1, 0, 0);
		commandList->EndRendering();

		const RHI::ResourceBarrierDesc beforePresent = {
			nullptr, backBuffer,
			RHI::ResourceState::RenderTarget, RHI::ResourceState::Present, {}
		};
		commandList->ResourceBarrier(&beforePresent, 1);
		commandList.Submit();
		resources.reflectionState = RHI::ResourceState::ShaderResource;
		device.Present();
	}
}

int main(int argc, char** argv)
{
	try
	{
		if(argc != 11) throw std::invalid_argument(
			"RayTracing raygen-bin entry miss-bin entry closest-hit-bin entry fullscreen-vs-bin entry present-fs-bin entry");
		const std::array<ShaderAsset, 5> shaders = {{
			{ argv[1], argv[2], RHI::ShaderStage::RayGeneration },
			{ argv[3], argv[4], RHI::ShaderStage::Miss },
			{ argv[5], argv[6], RHI::ShaderStage::ClosestHit },
			{ argv[7], argv[8], RHI::ShaderStage::Vertex },
			{ argv[9], argv[10], RHI::ShaderStage::Fragment }
		}};
		Platform::Window window(1280, 720, "RHI - Ray-traced Reflection");
		std::unique_ptr<dy::RHI::IDevice> device(
			dy::RHI::IDevice::Create(window.GetHandle()));
		Require(device != nullptr, "RHI device 생성에 실패했습니다.");
		if(!device->Supports(dy::RHI::Feature::RayTracingPipeline))
		{
			std::cout << "이 device는 Ray Tracing을 지원하지 않습니다.\n";
			return 2;
		}

		dy::RHI::SwapchainDesc swapchain = {};
		swapchain.format = dy::RHI::Format::B8G8R8A8_UNORM;
		swapchain.minimumImageCount = 2;
		swapchain.presentMode = dy::RHI::PresentMode::Fifo;
		Require(device->CreateSwapchain(swapchain), "swapchain 생성에 실패했습니다.");

		Resources resources;
		try
		{
			CreateGeometry(*device, resources);
			CreatePipelines(*device, swapchain.format, resources, shaders);
			while(window.IsRunning())
			{
				window.PollEvents();
				if(!window.IsRunning()) break;
				if(!device->BeginFrame()) continue;
				RenderFrame(*device, resources);
			}
		}
		catch(...)
		{
			resources.Destroy(*device);
			throw;
		}
		resources.Destroy(*device);
		return 0;
	}
	catch(const std::exception& exception)
	{
		std::cerr << exception.what() << '\n';
		return 1;
	}
}

// 필요한 공개 RHI 추가:
// - Feature::RayTracingPipeline, IDevice::Supports
// - RayGeneration/Miss/ClosestHit shader stage와 ShaderStageFlags::RayTracing
// - AccelerationStructureHandle 및 BLAS/TLAS desc·instance, 생성/파괴 API
// - scratch 크기 조회와 scratch buffer를 받는 ICommandList::BuildAccelerationStructure
// - ResourceBindingType::AccelerationStructure와 ResourceBinding의 AS handle
// - RayTracingPipelineDesc, IDevice::CreateRayTracingPipeline
// - ICommandList::BindRayTracingPipeline, TraceRays
// - AS build-input buffer usage/state와 BLAS -> TLAS -> trace dependency/barrier
// - ShaderTableDesc/Handle, CreateShaderTable/DestroyShaderTable: record와 hit group 명시
// - staged pipeline 미지원 device는 capability로 거부하며 ray query로 자동 대체하지 않는다
