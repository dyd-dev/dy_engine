#include "dyf.h"
#include "dyf/RHI.h"

#include "vertex.h"
#include "fragment.h"

#include <array>
#include <cstddef>
#include <memory>
#include <cstdio>
#include <iostream>
#include <chrono>

namespace
{
	// 이 예제에서 사용하는 방향광과 PBR 환경 입력만 전송한다.
	struct LightingConstants
	{
		dyf::Math::float4 cameraPosition;
		dyf::Math::float4 directionalLightDirection;
		dyf::Math::float4 directionalLightColor;
		dyf::Math::float4 ambientColor;
		dyf::Math::float4 pbrParams;
		dyf::Math::float4 environmentColor;
	};

	// 행렬과 재질 값의 C++ 배치를 각 셰이더의 상수 입력과 맞춘다.
	struct alignas(16) DrawConstants
	{
		dyf::Math::float4x4 viewProjectionMatrix;
		dyf::Math::float4x4 modelMatrix;
		dyf::Math::float4 baseColor;
		float metallic;
		float roughness;
	};
	static_assert(sizeof(LightingConstants) == 96);
	static_assert(offsetof(DrawConstants, baseColor) == 128);
	static_assert(offsetof(DrawConstants, metallic) == 144);
	static_assert(sizeof(DrawConstants) == 160);
}

int main()
{
	constexpr uint32_t windowWidth = 640, windowHeight = 480;
	dyf::Platform::Window window(windowWidth, windowHeight, "RHI / Scene");
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
	const dyf::RHI::VertexBufferLayout vertexLayout{0, sizeof(dyf::Vertex), dyf::RHI::VertexStepMode::Vertex};
	const std::array<dyf::RHI::VertexAttribute, 2> attributes = {{
		{0, 0, dyf::RHI::Format::R32G32B32_FLOAT, offsetof(dyf::Vertex, position)},
		{1, 0, dyf::RHI::Format::R32G32B32_FLOAT, offsetof(dyf::Vertex, normal)}
	}};
	const std::array<dyf::RHI::ResourceBindingLayout, 1> bindingLayout = {{
		{1, dyf::RHI::ResourceBindingType::ConstantBuffer, 1, dyf::RHI::ShaderStageFlags::Fragment, {}}
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
	lighting.cameraPosition = {0, -3, 1.6f, 0};
	lighting.directionalLightDirection = {0, -1, 2, 0};
	lighting.directionalLightColor = {1, 1, 1, 3};
	lighting.ambientColor = {1, 1, 1, 0.035f};
	lighting.pbrParams = {0.04f, 0.25f, 1, 0};
	lighting.environmentColor = {1, 1, 1, 1};

	auto* lightBuffer = resources.Keep(device.CreateBuffer({sizeof(lighting),
		sizeof(lighting), dyf::RHI::BufferUsage::Constant, dyf::RHI::ResourceState::CopyDestination}));
	if(!lightBuffer || !device.UpdateBuffer(*upload, lightBuffer, 0, &lighting, lightBuffer->GetDesc().size))
	{
		std::cerr << "Buffer upload failed." << '\n';
		return 1;
	}
	const dyf::RHI::ResourceBarrierDesc lightBufferReady{lightBuffer, nullptr, dyf::RHI::ResourceState::CopyDestination, dyf::RHI::ResourceState::ConstantBuffer, {}};
	upload->ResourceBarrier(&lightBufferReady, 1);
	std::array<DrawConstants, 3> draws{};
	for(auto& draw : draws)
	{
		draw.modelMatrix = dyf::Math::float4x4::Identity();
		draw.baseColor = {1, 1, 1, 1};
		draw.metallic = 0;
		draw.roughness = 0.5f;
	}
	draws[0].baseColor = {1, 0.3f, 0.2f, 1};
	draws[1].baseColor = {0.2f, 0.8f, 1, 1};
	draws[2].baseColor = {0.5f, 1, 0.3f, 1};
	if(!upload->Close() || !device.Submit(&upload, 1))
	{
		std::cerr << "Mesh upload submission failed." << '\n';
		return 1;
	}
	const std::array<dyf::RHI::ResourceBinding, 1> materialBindings = {{
		{1, 0, lightBuffer, nullptr, 0, sizeof(lighting), {}}
	}};
	auto* bindings = resources.Keep(device.CreateResourceSet({pipeline, materialBindings.data(), materialBindings.size()}));
	if(!bindings)
	{
		std::cerr << "Scene resource bindings failed.\n";
		return 1;
	}

	dyf::Camera camera;
	const dyf::Math::float3 cameraPosition = {0, -3, 1.6f};
	const dyf::Math::float3 cameraTarget = {0, 0, 0};
	camera.LookAt(cameraPosition, cameraTarget);
	camera.SetPerspective(depthDesc.width / static_cast<float>(depthDesc.height));
	for(auto& draw : draws)
	{
		draw.viewProjectionMatrix = camera.projection * camera.view;
	}
	float elapsedSeconds = 0;
	auto lastFrame = std::chrono::steady_clock::now();
	while(true)
	{
		window.PollEvents();
		if(!window.IsRunning()) break;

		const auto parentTransform = dyf::Math::RotationZ(elapsedSeconds);
		draws[0].modelMatrix = parentTransform * dyf::Math::Translation({-0.8f, 0, 0}) * dyf::Math::RotationX(0.4f);
		draws[1].modelMatrix = parentTransform * dyf::Math::Translation({0.6f, 0, 0.2f}) * dyf::Math::Scaling(0.6f);
		draws[2].modelMatrix = parentTransform * dyf::Math::Translation({0.6f, 0, 0.85f}) * dyf::Math::Scaling(0.3f);
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
		const auto now = std::chrono::steady_clock::now();
		const float deltaSeconds = std::chrono::duration<float>(now - lastFrame).count();
		lastFrame = now;
		elapsedSeconds += deltaSeconds;
	}
	return 0;
}
