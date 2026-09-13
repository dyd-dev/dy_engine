#include "dyf.h"
#include "dyf/RHI.h"

#include "vertex.h"
#include "fragment.h"
#include "shadow.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <cstdio>
#include <iostream>

namespace
{
	struct RendererDirectionalLight
	{
		dyf::Math::float4 directionIntensity;
		dyf::Math::float4 color;
	};

	struct RendererPointLight
	{
		dyf::Math::float4 positionRange;
		dyf::Math::float4 colorIntensity;
	};

	struct RendererSpotLight
	{
		dyf::Math::float4 positionRange;
		dyf::Math::float4 directionOuterCos;
		dyf::Math::float4 colorIntensity;
		dyf::Math::float4 coneParams;
	};

	struct RendererRectAreaLight
	{
		dyf::Math::float4 positionIntensity;
		dyf::Math::float4 directionWidth;
		dyf::Math::float4 upHeight;
		dyf::Math::float4 color;
	};

	struct RendererDiscAreaLight
	{
		dyf::Math::float4 positionIntensity;
		dyf::Math::float4 directionRadius;
		dyf::Math::float4 up;
		dyf::Math::float4 color;
	};

	struct LightingConstants
	{
		dyf::Math::float4 cameraPosition;
		dyf::Math::float4 directionalLightDirection;
		dyf::Math::float4 ambientColor;
		dyf::Math::float4 shadowParams;
		dyf::Math::float4 pbrParams;
		dyf::Math::float4 environmentColor;
		dyf::Math::float4 pointLightPositionRange;
		dyf::Math::float4 pointLightColorIntensity;
		dyf::Math::float4 lightCounts;
		dyf::Math::float4 areaLightCounts;
		RendererDirectionalLight directionalLights[1];
		RendererPointLight pointLights[1];
		RendererSpotLight spotLights[1];
		RendererRectAreaLight rectAreaLights[1];
		RendererDiscAreaLight discAreaLights[1];
	};

	static_assert(sizeof(LightingConstants) == 416);

	struct ShadowConstants
	{
		// 정적 예제의 방향광 1개·점광원 6면·스폿 광원 1개를 같은 깊이 아틀라스에 저장한다.
		dyf::Math::float4x4 lightViewProjectionMatrix[8];
		dyf::Math::float4 atlasRect[8];
		dyf::Math::float4 directionalViews[1];
		dyf::Math::float4 pointViews[1];
		dyf::Math::float4 spotViews[1];
	};
	static_assert(sizeof(ShadowConstants) == 688);

	struct DrawConstants
	{
		dyf::Math::float4x4 viewProjectionMatrix;
		dyf::Math::float4x4 modelMatrix;
		dyf::Math::float4 baseColor;
		float metallic = 0;
		float roughness = 0.65f;
		uint32_t receiveShadow = 1;
		uint32_t shadowViewIndex = 0;
	};

	static_assert(offsetof(DrawConstants, baseColor) == 128u);
	static_assert(offsetof(DrawConstants, metallic) == 144u);
	static_assert(offsetof(DrawConstants, shadowViewIndex) == 156u);
	static_assert(sizeof(DrawConstants) == 160u);
}

int main()
{
	constexpr uint32_t windowWidth = 640, windowHeight = 480;
	dyf::Platform::Window window(windowWidth, windowHeight, "RHI / Five light types and shadows");
	if(!window.GetHandle())
	{
		std::cerr << "Window creation failed.\n";
		return 1;
	}
	std::unique_ptr<dyf::RHI::IDevice> deviceOwner(dyf::RHI::IDevice::Create(dyf::RHI::DeviceDesc{}));
	if(!deviceOwner)
	{
		std::cerr << "RHI device creation failed." << '\n';
		return 1;
	}
	auto& device = *deviceOwner;
	dyf::RHI::ResourceScope resources(device);
	dyf::RHI::SwapchainDesc swapchain;
	swapchain.window = window.GetHandle();
	swapchain.format = dyf::RHI::Format::B8G8R8A8_UNORM;
	swapchain.minimumImageCount = 2;
	swapchain.presentMode = dyf::RHI::PresentMode::Fifo;
	if(!device.CreateSwapchain(swapchain))
	{
		std::cerr << "Swapchain creation failed." << '\n';
		return 1;
	}
	auto* vertexShader = resources.Keep(device.CreateShader({dyf::RHI::ShaderStage::Vertex,
		ShaderData::vertexEntryPoint, ShaderData::vertex, ShaderData::vertexSize}));
	auto* fragmentShader = resources.Keep(device.CreateShader({dyf::RHI::ShaderStage::Fragment,
		ShaderData::fragmentEntryPoint, ShaderData::fragment, ShaderData::fragmentSize}));
	auto* shadowShader = resources.Keep(device.CreateShader({dyf::RHI::ShaderStage::Vertex,
		ShaderData::shadowEntryPoint, ShaderData::shadow, ShaderData::shadowSize}));
	const dyf::RHI::VertexBufferLayout vertexLayout{0, sizeof(dyf::Vertex), dyf::RHI::VertexStepMode::Vertex};
	const std::array<dyf::RHI::VertexAttribute, 2> attributes = {{
		{0, 0, dyf::RHI::Format::R32G32B32_FLOAT, offsetof(dyf::Vertex, position)},
		{1, 0, dyf::RHI::Format::R32G32B32_FLOAT, offsetof(dyf::Vertex, normal)}
	}};
	dyf::RHI::SamplerDesc shadowSampler;
	shadowSampler.minFilter = dyf::RHI::SamplerFilter::Nearest;
	shadowSampler.magFilter = dyf::RHI::SamplerFilter::Nearest;
	shadowSampler.mipFilter = dyf::RHI::SamplerFilter::Nearest;
	shadowSampler.addressU = dyf::RHI::SamplerAddressMode::ClampToEdge;
	shadowSampler.addressV = dyf::RHI::SamplerAddressMode::ClampToEdge;
	shadowSampler.addressW = dyf::RHI::SamplerAddressMode::ClampToEdge;
	shadowSampler.minLod = 0;
	shadowSampler.maxLod = 0;
	shadowSampler.mipLodBias = 0;
	const std::array<dyf::RHI::ResourceBindingLayout, 4> bindingLayout = {{
		{1, dyf::RHI::ResourceBindingType::ConstantBuffer, 1, dyf::RHI::ShaderStageFlags::Fragment, {}},
		{2, dyf::RHI::ResourceBindingType::SampledTexture, 1, dyf::RHI::ShaderStageFlags::Fragment, {}},
		{3, dyf::RHI::ResourceBindingType::ConstantBuffer, 1, dyf::RHI::ShaderStageFlags::Fragment, {}},
		{9, dyf::RHI::ResourceBindingType::StaticSampler, 1, dyf::RHI::ShaderStageFlags::Fragment, shadowSampler}
	}};
	const dyf::RHI::ColorAttachmentDesc colorOutput{swapchain.format,
		{true, dyf::RHI::BlendFactor::SourceAlpha, dyf::RHI::BlendFactor::OneMinusSourceAlpha, dyf::RHI::BlendOp::Add,
		dyf::RHI::BlendFactor::One, dyf::RHI::BlendFactor::Zero, dyf::RHI::BlendOp::Add}, dyf::RHI::ColorWriteMask::All};
	dyf::RHI::GraphicsPipelineDesc pipelineDesc;
	pipelineDesc.vertexShader = vertexShader;
	pipelineDesc.fragmentShader = fragmentShader;
	pipelineDesc.topology = dyf::RHI::PrimitiveTopology::TriangleList;
	pipelineDesc.vertexBuffers = &vertexLayout;
	pipelineDesc.vertexBufferCount = 1;
	pipelineDesc.vertexAttributes = attributes.data();
	pipelineDesc.vertexAttributeCount = attributes.size();
	pipelineDesc.raster = {dyf::RHI::FillMode::Solid, dyf::RHI::CullMode::Back, dyf::RHI::FrontFace::CounterClockwise, 0, 0, 0};
	pipelineDesc.depthStencil.format = dyf::RHI::Format::D32_FLOAT;
	pipelineDesc.depthStencil.depthWriteEnabled = true;
	pipelineDesc.depthStencil.depthTestEnabled = true;
	pipelineDesc.depthStencil.depthCompareOp = dyf::RHI::CompareOp::Less;
	pipelineDesc.colorAttachments = &colorOutput;
	pipelineDesc.colorAttachmentCount = 1;
	pipelineDesc.layout = {bindingLayout.data(), static_cast<uint32_t>(bindingLayout.size()), sizeof(DrawConstants),
		dyf::RHI::ShaderStageFlags::Vertex | dyf::RHI::ShaderStageFlags::Fragment, 10};
	auto* pipeline = resources.Keep(device.CreateGraphicsPipeline(pipelineDesc));
	if(!pipeline)
	{
		std::cerr << "Mesh pipeline creation failed." << '\n';
		return 1;
	}
	// 정점 위치와 객체 변환만으로 색 출력 없는 깊이 패스를 구성한다.
	const dyf::RHI::ResourceBindingLayout shadowLayout{3, dyf::RHI::ResourceBindingType::ConstantBuffer, 1, dyf::RHI::ShaderStageFlags::Vertex, {}};
	auto shadowDesc = pipelineDesc;
	shadowDesc.vertexShader = shadowShader;
	shadowDesc.fragmentShader = nullptr;
	shadowDesc.colorAttachments = nullptr;
	shadowDesc.colorAttachmentCount = 0;
	shadowDesc.vertexAttributeCount = 1;
	shadowDesc.raster = {dyf::RHI::FillMode::Solid, dyf::RHI::CullMode::None, dyf::RHI::FrontFace::CounterClockwise, 0, 1.75f, 0};
	shadowDesc.layout = {&shadowLayout, 1, sizeof(DrawConstants), dyf::RHI::ShaderStageFlags::Vertex, 10};
	auto* shadowPipeline = resources.Keep(device.CreateGraphicsPipeline(shadowDesc));
	if(!shadowPipeline)
	{
		std::cerr << "Shadow pipeline creation failed." << '\n';
		return 1;
	}
	constexpr uint32_t shadowColumns = 4, shadowRows = 2, shadowViews = 8;
	const auto shadowResolution = static_cast<uint32_t>(std::min<uint64_t>(1024,
		device.GetLimit(dyf::RHI::Limit::Texture2DDimension) / shadowColumns));
	if(!shadowResolution)
	{
		std::cerr << "Shadow atlas size is unsupported." << '\n';
		return 1;
	}
	dyf::RHI::TextureDesc shadowTextureDesc;
	shadowTextureDesc.width = shadowResolution * shadowColumns;
	shadowTextureDesc.height = shadowResolution * shadowRows;
	shadowTextureDesc.format = dyf::RHI::Format::D32_FLOAT;
	shadowTextureDesc.usage = dyf::RHI::TextureUsage::DepthStencil | dyf::RHI::TextureUsage::ShaderResource;
	auto* shadowTexture = resources.Keep(device.CreateTexture(shadowTextureDesc));
	if(!shadowTexture)
	{
		std::cerr << "Shadow atlas creation failed." << '\n';
		return 1;
	}
	dyf::RHI::ResourceState shadowState = dyf::RHI::ResourceState::Undefined;
	dyf::RHI::TextureDesc depthDesc;
	depthDesc.width = device.GetBackBuffer()->GetDesc().width;
	depthDesc.height = device.GetBackBuffer()->GetDesc().height;
	depthDesc.format = dyf::RHI::Format::D32_FLOAT;
	depthDesc.usage = dyf::RHI::TextureUsage::DepthStencil;
	const auto destroyDepth = [&device](dyf::RHI::Texture* texture) { device.DestroyTexture(texture); };
	std::unique_ptr<dyf::RHI::Texture, decltype(destroyDepth)> depthOwner(device.CreateTexture(depthDesc), destroyDepth);
	auto* depthTexture = depthOwner.get();
	if(!depthTexture)
	{
		std::cerr << "Mesh depth target creation failed." << '\n';
		return 1;
	}
	dyf::RHI::ResourceState depthState = dyf::RHI::ResourceState::Undefined;
	const auto mesh = dyf::CreateCubeMesh(1);

	dyf::RHI::ResourceScope uploadScope(device);
	auto* upload = uploadScope.Keep(device.AcquireCommandList());
	if(!upload)
	{
		std::cerr << "Upload command list unavailable." << '\n';
		return 1;
	}
	const auto verticesBytes = mesh.vertices.size() * sizeof(dyf::Vertex);
	if(verticesBytes > UINT32_MAX)
	{
		std::cerr << "Buffer data exceeds the RHI size range." << '\n';
		return 1;
	}
	auto* vertices = resources.Keep(device.CreateBuffer({static_cast<uint32_t>(verticesBytes),
		sizeof(dyf::Vertex), dyf::RHI::BufferUsage::Vertex, dyf::RHI::ResourceState::CopyDestination}));
	if(!vertices || !device.UpdateBuffer(*upload, vertices, 0, mesh.vertices.data(), vertices->GetDesc().size))
	{
		std::cerr << "Buffer upload failed." << '\n';
		return 1;
	}
	const dyf::RHI::ResourceBarrierDesc verticesReady{vertices, nullptr, dyf::RHI::ResourceState::CopyDestination, dyf::RHI::ResourceState::VertexBuffer, {}};
	upload->ResourceBarrier(&verticesReady, 1);
	const auto indicesBytes = mesh.indices.size() * sizeof(uint32_t);
	if(indicesBytes > UINT32_MAX)
	{
		std::cerr << "Buffer data exceeds the RHI size range." << '\n';
		return 1;
	}
	auto* indices = resources.Keep(device.CreateBuffer({static_cast<uint32_t>(indicesBytes),
		sizeof(uint32_t), dyf::RHI::BufferUsage::Index, dyf::RHI::ResourceState::CopyDestination}));
	if(!indices || !device.UpdateBuffer(*upload, indices, 0, mesh.indices.data(), indices->GetDesc().size))
	{
		std::cerr << "Buffer upload failed." << '\n';
		return 1;
	}
	const dyf::RHI::ResourceBarrierDesc indicesReady{indices, nullptr, dyf::RHI::ResourceState::CopyDestination, dyf::RHI::ResourceState::IndexBuffer, {}};
	upload->ResourceBarrier(&indicesReady, 1);
	LightingConstants lighting{};
	lighting.cameraPosition = {2.6f, -4, 2.7f, 0};
	lighting.directionalLightDirection = {0, -1, 2, 0};
	lighting.ambientColor = {1, 1, 1, 0.035f};
	lighting.shadowParams = {0.0007f, 0.003f, 0, 1};
	lighting.pbrParams = {0.04f, 0.25f, 1, 1};
	lighting.environmentColor = {1, 1, 1, 1};
	const auto direction = dyf::Math::NormalizeOr({0, -1, 2}, {0, 0, -1});
	lighting.directionalLights[0] = {{direction.x, direction.y, direction.z, 0.3f}, {1, 1, 1, 0}};

	lighting.pointLightPositionRange = {-1, -1, 1.4f, 5};
	lighting.pointLightColorIntensity = {1, 0.2f, 0.1f, 3};
	lighting.pointLights[0] = {lighting.pointLightPositionRange, lighting.pointLightColorIntensity};
	const auto spotDirection = dyf::Math::NormalizeOr({-0.6f, 0.3f, -1}, {0, 0, -1});
	lighting.spotLights[0] = {{1, -1, 2, 5}, {spotDirection.x, spotDirection.y, spotDirection.z, std::cos(0.8f)},
		{0.2f, 0.3f, 1, 7}, {std::cos(0.35f), 0, 0, 0}};
	lighting.rectAreaLights[0] = {{0, 1.4f, 2, 4}, {0, 0, -1, 2}, {0, 1, 0, 1}, {1, 0.7f, 0.2f, 0}};
	lighting.discAreaLights[0] = {{-1.5f, 0, 2, 4}, {0, 0, -1, 0.7f}, {0, 1, 0, 0}, {0.2f, 1, 0.8f, 0}};
	lighting.lightCounts = {1, 1, 1, 1};
	lighting.areaLightCounts = {1, 1, 0, 0};
	// 장면과 광원이 고정되어 있으므로 광원 행렬은 초기화할 때 한 번만 계산한다.
	// 방향광은 장면을 담는 직교 뷰, 점광원은 여섯 면, 스폿 광원은 원뿔의 투시 뷰다.
	ShadowConstants shadows{};
	auto directionalProjection = dyf::Math::OrthographicRH_ZO(4, 4, .1f, 20);
	directionalProjection.m[5] = -directionalProjection.m[5];
	shadows.lightViewProjectionMatrix[0] = directionalProjection *
		dyf::Math::LookAtRH(direction * 8, {0, 0, 0}, {0, 0, 1});
	shadows.directionalViews[0] = {0, 1, .45f, 0};
	const dyf::Math::float3 pointPosition{-1, -1, 1.4f};
	const std::array<dyf::Math::float3, 6> faceDirections = {{{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
	const std::array<dyf::Math::float3, 6> faceUp = {{{0, 0, -1}, {0, 0, -1}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}}};
	const auto pointProjection = dyf::Math::PerspectiveRH_ZO(1.57079633f, 1, .1f, 5);
	for(uint32_t face = 0; face < faceDirections.size(); ++face)
	{
		shadows.lightViewProjectionMatrix[1 + face] = pointProjection *
			dyf::Math::LookAtRH(pointPosition, pointPosition + faceDirections[face], faceUp[face]);
	}
	shadows.pointViews[0] = {1, 6, .5f, 0};
	const dyf::Math::float3 spotPosition{1, -1, 2};
	auto spotProjection = dyf::Math::PerspectiveRH_ZO(1.6f, 1, .1f, 5);
	spotProjection.m[5] = -spotProjection.m[5];
	shadows.lightViewProjectionMatrix[7] = spotProjection *
		dyf::Math::LookAtRH(spotPosition, spotPosition + spotDirection, {0, 0, 1});
	shadows.spotViews[0] = {7, 1, 1, 0};
	for(uint32_t shadowViewIndex = 0; shadowViewIndex < shadowViews; ++shadowViewIndex)
	{
		shadows.atlasRect[shadowViewIndex] = {
			static_cast<float>(shadowViewIndex % shadowColumns) / shadowColumns,
			static_cast<float>(shadowViewIndex / shadowColumns) / shadowRows,
			1.f / shadowColumns, 1.f / shadowRows
		};
	}
	auto* shadowBuffer = resources.Keep(device.CreateBuffer({sizeof(shadows), sizeof(shadows),
		dyf::RHI::BufferUsage::Constant, dyf::RHI::ResourceState::CopyDestination}));
	if(!shadowBuffer || !device.UpdateBuffer(*upload, shadowBuffer, 0, &shadows, sizeof(shadows)))
	{
		std::cerr << "Shadow constants upload failed." << '\n';
		return 1;
	}
	const dyf::RHI::ResourceBarrierDesc shadowBufferReady{shadowBuffer, nullptr, dyf::RHI::ResourceState::CopyDestination, dyf::RHI::ResourceState::ConstantBuffer, {}};
	upload->ResourceBarrier(&shadowBufferReady, 1);
	auto* lightBuffer = resources.Keep(device.CreateBuffer({sizeof(lighting),
		sizeof(lighting), dyf::RHI::BufferUsage::Constant, dyf::RHI::ResourceState::CopyDestination}));
	if(!lightBuffer || !device.UpdateBuffer(*upload, lightBuffer, 0, &lighting, lightBuffer->GetDesc().size))
	{
		std::cerr << "Buffer upload failed." << '\n';
		return 1;
	}
	const dyf::RHI::ResourceBarrierDesc lightBufferReady{lightBuffer, nullptr, dyf::RHI::ResourceState::CopyDestination, dyf::RHI::ResourceState::ConstantBuffer, {}};
	upload->ResourceBarrier(&lightBufferReady, 1);
	// 이미지가 없는 슬롯은 플래그가 꺼져 있어 샘플링하지 않는다. 바인딩만 흰색 텍스처로 채운다.
	std::array<DrawConstants, 3> draws{};
	for(auto& draw : draws)
	{
		draw.modelMatrix = dyf::Math::float4x4::Identity();
		draw.receiveShadow = 1;
		draw.baseColor = {1, 1, 1, 1};
		draw.metallic = 0;
		draw.roughness = 0.65f;
	}
	draws[0].modelMatrix = dyf::Math::Translation({-0.7f, 0, 0});
	draws[1].modelMatrix = dyf::Math::Translation({0.7f, 0, 0});
	draws[2].modelMatrix = dyf::Math::Translation({0, 0, -0.65f}) * dyf::Math::Scaling(dyf::Math::float3{3, 3, 0.1f});
	if(!upload->Close() || !device.Submit(&upload, 1))
	{
		std::cerr << "Mesh upload submission failed." << '\n';
		return 1;
	}
	const std::array<dyf::RHI::ResourceBinding, 3> materialBindings = {{
		{1, 0, lightBuffer, nullptr, 0, sizeof(LightingConstants), {}},
		{2, 0, nullptr, shadowTexture, 0, 0, {}},
		{3, 0, shadowBuffer, nullptr, 0, sizeof(shadows), {}}
	}};
	auto* bindings = resources.Keep(device.CreateResourceSet({pipeline, materialBindings.data(), materialBindings.size()}));
	const dyf::RHI::ResourceBinding shadowBinding{3, 0, shadowBuffer, nullptr, 0, sizeof(shadows), {}};
	auto* shadowBindings = resources.Keep(device.CreateResourceSet({shadowPipeline, &shadowBinding, 1}));
	if(!bindings || !shadowBindings)
	{
		std::cerr << "Light bindings creation failed." << '\n';
		return 1;
	}

	dyf::Camera camera;
	const dyf::Math::float3 cameraPosition = {2.6f, -4, 2.7f};
	const dyf::Math::float3 cameraTarget = {0, 0, 0};
	camera.LookAt(cameraPosition, cameraTarget);
	camera.SetPerspective(depthDesc.width / static_cast<float>(depthDesc.height));
	for(auto& draw : draws)
	{
		draw.viewProjectionMatrix = camera.projection * camera.view;
	}
	while(true)
	{
		window.PollEvents();
		if(!window.IsRunning()) break;

		if(!device.BeginFrame()) continue;
		// BeginFrame이 교체한 backbuffer와 depth target의 크기를 맞춘다.
		const auto& frameTargetDesc = device.GetBackBuffer()->GetDesc();
		if (depthDesc.width != frameTargetDesc.width || depthDesc.height != frameTargetDesc.height)
		{
			depthDesc.width = frameTargetDesc.width;
			depthDesc.height = frameTargetDesc.height;
			auto* resizedDepth = device.CreateTexture(depthDesc);
			if (!resizedDepth)
			{
				std::fprintf(stderr, "Resized depth target creation failed.\n");
				return 1;
			}
			depthOwner.reset(resizedDepth);
			depthTexture = depthOwner.get();
			depthState = dyf::RHI::ResourceState::Undefined;
			camera.SetPerspective(depthDesc.width / static_cast<float>(depthDesc.height));
			for (auto& draw : draws) draw.viewProjectionMatrix = camera.projection * camera.view;
		}
		dyf::RHI::ResourceScope commandsScope(device);
		// 한 프레임 안에서 깊이 아틀라스를 먼저 제출하고 다음 제출에서 조명 계산에 사용한다.
		auto* shadowCommands = commandsScope.Keep(device.AcquireCommandList());
		if(!shadowCommands)
		{
			std::cerr << "Shadow command list unavailable." << '\n';
			return 1;
		}
		const dyf::RHI::ResourceBarrierDesc shadowBefore{nullptr, shadowTexture, shadowState, dyf::RHI::ResourceState::DepthWrite, {}};
		shadowCommands->ResourceBarrier(&shadowBefore, 1);
		dyf::RHI::DepthStencilAttachment shadowDepth;
		shadowDepth.texture = shadowTexture;
		shadowDepth.state = dyf::RHI::ResourceState::DepthWrite;
		shadowDepth.depthLoadOp = dyf::RHI::LoadOp::Clear;
		shadowDepth.depthStoreOp = dyf::RHI::StoreOp::Store;
		shadowDepth.clearDepth = 1;
		shadowCommands->BeginRendering({nullptr, 0, &shadowDepth});
		shadowCommands->BindGraphicsPipeline(shadowPipeline);
		shadowCommands->BindResourceSet(shadowBindings);
		shadowCommands->BindVertexBuffer(0, vertices, 0);
		shadowCommands->BindIndexBuffer(indices, dyf::RHI::Format::R32_UINT, 0);
		for(uint32_t shadowViewIndex = 0; shadowViewIndex < shadowViews; ++shadowViewIndex)
		{
			const auto atlasLeft = (shadowViewIndex % shadowColumns) * shadowResolution;
			const auto atlasTop = (shadowViewIndex / shadowColumns) * shadowResolution;
			shadowCommands->SetViewport({static_cast<float>(atlasLeft), static_cast<float>(atlasTop),
				static_cast<float>(shadowResolution), static_cast<float>(shadowResolution), 0, 1});
			shadowCommands->SetScissor({static_cast<int32_t>(atlasLeft), static_cast<int32_t>(atlasTop), shadowResolution, shadowResolution});
			for(auto draw : draws)
			{
				draw.shadowViewIndex = shadowViewIndex;
				shadowCommands->SetInlineConstants(0, sizeof(draw), &draw);
				shadowCommands->DrawIndexedInstanced(mesh.indices.size(), 1, 0, 0, 0);
			}
		}
		shadowCommands->EndRendering();
		const dyf::RHI::ResourceBarrierDesc shadowReady{nullptr, shadowTexture, dyf::RHI::ResourceState::DepthWrite, dyf::RHI::ResourceState::ShaderResource, {}};
		shadowCommands->ResourceBarrier(&shadowReady, 1);
		if(!shadowCommands->Close() || !device.Submit(&shadowCommands, 1))
		{
			std::cerr << "Shadow submission failed." << '\n';
			return 1;
		}
		shadowState = dyf::RHI::ResourceState::ShaderResource;
		auto* commands = commandsScope.Keep(device.AcquireCommandList());
		if(!commands)
		{
			std::cerr << "Draw command list unavailable." << '\n';
			return 1;
		}
		auto* backBuffer = device.GetBackBuffer();
		const std::array<dyf::RHI::ResourceBarrierDesc, 2> before = {{
			{nullptr, backBuffer, dyf::RHI::ResourceState::Present, dyf::RHI::ResourceState::RenderTarget, {}},
			{nullptr, depthTexture, depthState, dyf::RHI::ResourceState::DepthWrite, {}}
		}};
		commands->ResourceBarrier(before.data(), depthState == dyf::RHI::ResourceState::DepthWrite ? 1 : 2);
		dyf::RHI::ColorAttachment color;
		color.texture = backBuffer;
		color.loadOp = dyf::RHI::LoadOp::Clear;
		color.storeOp = dyf::RHI::StoreOp::Store;
		color.clearColor[0] = 0.08f;
		color.clearColor[1] = 0.10f;
		color.clearColor[2] = 0.14f;
		color.clearColor[3] = 1;
		dyf::RHI::DepthStencilAttachment depth;
		depth.texture = depthTexture;
		depth.state = dyf::RHI::ResourceState::DepthWrite;
		depth.depthLoadOp = dyf::RHI::LoadOp::Clear;
		depth.depthStoreOp = dyf::RHI::StoreOp::Discard;
		depth.clearDepth = 1;
		commands->BeginRendering({&color, 1, &depth});
		commands->BindGraphicsPipeline(pipeline);
		commands->BindResourceSet(bindings);
		commands->BindVertexBuffer(0, vertices, 0);
		commands->BindIndexBuffer(indices, dyf::RHI::Format::R32_UINT, 0);
		const dyf::RHI::Viewport viewport{0, 0, static_cast<float>(backBuffer->GetDesc().width), static_cast<float>(backBuffer->GetDesc().height), 0, 1};
		const dyf::RHI::Rect scissor{0, 0, backBuffer->GetDesc().width, backBuffer->GetDesc().height};
		commands->SetViewport(viewport);
		commands->SetScissor(scissor);
		for(const auto& draw : draws)
		{
			commands->SetInlineConstants(0, sizeof(draw), &draw);
			commands->DrawIndexedInstanced(mesh.indices.size(), 1, 0, 0, 0);
		}
		commands->EndRendering();
		const dyf::RHI::ResourceBarrierDesc present{nullptr, backBuffer, dyf::RHI::ResourceState::RenderTarget, dyf::RHI::ResourceState::Present, {}};
		commands->ResourceBarrier(&present, 1);
		if(!commands->Close() || !device.Submit(&commands, 1))
		{
			std::cerr << "Mesh draw submission failed." << '\n';
			return 1;
		}
		depthState = dyf::RHI::ResourceState::DepthWrite;
		if(!device.Present())
		{
			std::cerr << "Presentation failed.\n";
			return 1;
		}
	}
	return 0;
}
