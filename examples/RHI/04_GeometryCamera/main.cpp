#include "dyf.h"
#include "dyf/RHI.h"

#include "vertex.h"
#include "fragment.h"

#include <array>
#include <cstddef>
#include <memory>
#include <cstdio>

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
	const uint32_t width = 640, height = 480;
	dyf::Platform::Window window(width, height, "RHI / GeometryCamera");
	if (!window.GetHandle()) return 1;

	std::unique_ptr<dyf::RHI::IDevice> deviceOwner(dyf::RHI::IDevice::Create(dyf::RHI::DeviceDesc{}));
	if (!deviceOwner)
	{
		std::fprintf(stderr, "RHI device creation failed.\n");
		return 1;
	}
	auto& device = *deviceOwner;
	dyf::RHI::ResourceScope resources(device);
	dyf::RHI::SwapchainDesc swapchain;
	swapchain.window = window.GetHandle();
	swapchain.format = dyf::RHI::Format::B8G8R8A8_UNORM;
	swapchain.minimumImageCount = 2;
	swapchain.presentMode = dyf::RHI::PresentMode::Fifo;
	if (!device.CreateSwapchain(swapchain))
	{
		std::fprintf(stderr, "Swapchain creation failed.\n");
		return 1;
	}
	auto* vertexShader = resources.Keep(device.CreateShader({dyf::RHI::ShaderStage::Vertex,
		ShaderData::vertexEntryPoint, ShaderData::vertex, ShaderData::vertexSize}));
	auto* fragmentShader = resources.Keep(device.CreateShader({dyf::RHI::ShaderStage::Fragment,
		ShaderData::fragmentEntryPoint, ShaderData::fragment, ShaderData::fragmentSize}));
	if (!vertexShader || !fragmentShader)
	{
		std::fprintf(stderr, "Shader creation failed.\n");
		return 1;
	}
	const dyf::RHI::VertexBufferLayout input{0, sizeof(dyf::Vertex), dyf::RHI::VertexStepMode::Vertex};
	const std::array<dyf::RHI::VertexAttribute, 2> attributes = {{
		{0, 0, dyf::RHI::Format::R32G32B32_FLOAT, offsetof(dyf::Vertex, position)},
		{1, 0, dyf::RHI::Format::R32G32B32_FLOAT, offsetof(dyf::Vertex, normal)}
	}};
	const std::array<dyf::RHI::ResourceBindingLayout, 1> bindingLayout = {{
		{1, dyf::RHI::ResourceBindingType::ConstantBuffer, 1, dyf::RHI::ShaderStageFlags::Fragment, {}}
	}};
	const dyf::RHI::ColorAttachmentDesc output{swapchain.format,
		{true, dyf::RHI::BlendFactor::SourceAlpha, dyf::RHI::BlendFactor::OneMinusSourceAlpha, dyf::RHI::BlendOp::Add,
			dyf::RHI::BlendFactor::One, dyf::RHI::BlendFactor::Zero, dyf::RHI::BlendOp::Add}, dyf::RHI::ColorWriteMask::All};
	dyf::RHI::GraphicsPipelineDesc pipelineDesc;
	pipelineDesc.vertexShader = vertexShader;
	pipelineDesc.fragmentShader = fragmentShader;
	pipelineDesc.topology = dyf::RHI::PrimitiveTopology::TriangleList;
	pipelineDesc.vertexBuffers = &input;
	pipelineDesc.vertexBufferCount = 1;
	pipelineDesc.vertexAttributes = attributes.data();
	pipelineDesc.vertexAttributeCount = attributes.size();
	pipelineDesc.raster = {dyf::RHI::FillMode::Solid, dyf::RHI::CullMode::Back, dyf::RHI::FrontFace::CounterClockwise, 0, 0, 0};
	pipelineDesc.depthStencil.format = dyf::RHI::Format::D32_FLOAT;
	pipelineDesc.depthStencil.depthTestEnabled = pipelineDesc.depthStencil.depthWriteEnabled = true;
	pipelineDesc.depthStencil.depthCompareOp = dyf::RHI::CompareOp::Less;
	pipelineDesc.colorAttachments = &output;
	pipelineDesc.colorAttachmentCount = 1;
	pipelineDesc.layout = {bindingLayout.data(), static_cast<uint32_t>(bindingLayout.size()), sizeof(DrawConstants),
		dyf::RHI::ShaderStageFlags::Vertex | dyf::RHI::ShaderStageFlags::Fragment, 10};
	auto* pipeline = resources.Keep(device.CreateGraphicsPipeline(pipelineDesc));
	if (!pipeline)
	{
		std::fprintf(stderr, "Mesh pipeline creation failed.\n");
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
	if (!depthTexture)
	{
		std::fprintf(stderr, "Mesh depth target creation failed.\n");
		return 1;
	}
	dyf::RHI::ResourceState depthState = dyf::RHI::ResourceState::Undefined;
	dyf::MeshData mesh;
	const std::array<dyf::Math::float3, 3> positions = {{{-0.9f, 0, -0.6f}, {0.9f, 0, -0.6f}, {0, 0, 0.9f}}};
	for (const auto& position : positions)
	{
		mesh.vertices.push_back({position, {0, -1, 0}, {0, 0}, {1, 1, 1, 1}, {1, 0, 0, 1}});
	}
	mesh.indices = {0, 1, 2};

	dyf::RHI::ResourceScope uploadScope(device);
	auto* upload = uploadScope.Keep(device.AcquireCommandList());
	if (!upload)
	{
		std::fprintf(stderr, "Upload command list unavailable.\n");
		return 1;
	}
	const auto verticesBytes = mesh.vertices.size() * sizeof(dyf::Vertex);
	if (verticesBytes > UINT32_MAX)
	{
		std::fprintf(stderr, "Buffer data exceeds the RHI size range.\n");
		return 1;
	}
	auto* vertices = resources.Keep(device.CreateBuffer({static_cast<uint32_t>(verticesBytes),
		sizeof(dyf::Vertex), dyf::RHI::BufferUsage::Vertex, dyf::RHI::ResourceState::CopyDestination}));
	if (!vertices)
	{
		std::fprintf(stderr, "Resource creation failed.\n");
		return 1;
	}
	if (!device.UpdateBuffer(*upload, vertices, 0, mesh.vertices.data(), vertices->GetDesc().size))
	{
		std::fprintf(stderr, "Buffer upload failed.\n");
		return 1;
	}
	const dyf::RHI::ResourceBarrierDesc verticesReady{vertices, nullptr,
		dyf::RHI::ResourceState::CopyDestination, dyf::RHI::ResourceState::VertexBuffer, {}};
	upload->ResourceBarrier(&verticesReady, 1);
	const auto indicesBytes = mesh.indices.size() * sizeof(uint32_t);
	if (indicesBytes > UINT32_MAX)
	{
		std::fprintf(stderr, "Buffer data exceeds the RHI size range.\n");
		return 1;
	}
	auto* indices = resources.Keep(device.CreateBuffer({static_cast<uint32_t>(indicesBytes),
		sizeof(uint32_t), dyf::RHI::BufferUsage::Index, dyf::RHI::ResourceState::CopyDestination}));
	if (!indices)
	{
		std::fprintf(stderr, "Resource creation failed.\n");
		return 1;
	}
	if (!device.UpdateBuffer(*upload, indices, 0, mesh.indices.data(), indices->GetDesc().size))
	{
		std::fprintf(stderr, "Buffer upload failed.\n");
		return 1;
	}
	const dyf::RHI::ResourceBarrierDesc indicesReady{indices, nullptr,
		dyf::RHI::ResourceState::CopyDestination, dyf::RHI::ResourceState::IndexBuffer, {}};
	upload->ResourceBarrier(&indicesReady, 1);

	// 조명 값을 셰이더의 상수 배치에 맞춰 직접 전달한다.
	LightingConstants lighting{};
	lighting.cameraPosition = {0, -3, 1.6f, 0};
	lighting.directionalLightDirection = {0, -1, 2, 0};
	lighting.directionalLightColor = {1, 1, 1, 3};
	lighting.ambientColor = {1, 1, 1, 0.035f};
	lighting.pbrParams = {0.04f, 0.25f, 1, 0};
	lighting.environmentColor = {1, 1, 1, 1};

	auto* lightBuffer = resources.Keep(device.CreateBuffer({sizeof(lighting),
		sizeof(lighting), dyf::RHI::BufferUsage::Constant, dyf::RHI::ResourceState::CopyDestination}));
	if (!lightBuffer)
	{
		std::fprintf(stderr, "Resource creation failed.\n");
		return 1;
	}
	if (!device.UpdateBuffer(*upload, lightBuffer, 0, &lighting, lightBuffer->GetDesc().size))
	{
		std::fprintf(stderr, "Buffer upload failed.\n");
		return 1;
	}
	const dyf::RHI::ResourceBarrierDesc lightBufferReady{lightBuffer, nullptr,
		dyf::RHI::ResourceState::CopyDestination, dyf::RHI::ResourceState::ConstantBuffer, {}};
	upload->ResourceBarrier(&lightBufferReady, 1);
	DrawConstants draw{};
	draw.modelMatrix = dyf::Math::float4x4::Identity();
	draw.baseColor = {0.2f, 0.7f, 1, 1};
	draw.metallic = 0;
	draw.roughness = 0.5f;
	upload->Close();
	if (!device.Submit(&upload, 1))
	{
		std::fprintf(stderr, "Mesh upload submission failed.\n");
		return 1;
	}
	const std::array<dyf::RHI::ResourceBinding, 1> materialBindings = {{
		{1, 0, lightBuffer, nullptr, 0, sizeof(lighting), {}}
	}};
	auto* bindings = resources.Keep(device.CreateResourceSet({pipeline, materialBindings.data(), materialBindings.size()}));
	if (!bindings)
	{
		std::fprintf(stderr, "Resource bindings failed.\n");
		return 1;
	}

	dyf::Camera camera;
	const dyf::Math::float3 cameraPosition = {0, -3, 1.6f};
	const dyf::Math::float3 cameraTarget = {0, 0, 0};
	camera.LookAt(cameraPosition, cameraTarget);
	camera.SetPerspective(depthDesc.width / static_cast<float>(depthDesc.height));
	draw.viewProjectionMatrix = camera.projection * camera.view;
	const dyf::Math::float4 clearColor = {0.08f, 0.10f, 0.14f, 1};
	while (true)
	{
		window.PollEvents();
		if (!window.IsRunning()) break;
		if (!device.BeginFrame()) continue;
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
			draw.viewProjectionMatrix = camera.projection * camera.view;
		}

		dyf::RHI::ResourceScope commandsScope(device);
		auto* commands = commandsScope.Keep(device.AcquireCommandList());
		if (!commands)
		{
			std::fprintf(stderr, "Draw command list unavailable.\n");
			return 1;
		}
		auto* target = device.GetBackBuffer();
		const std::array<dyf::RHI::ResourceBarrierDesc, 2> before = {{
			{nullptr, target, dyf::RHI::ResourceState::Present, dyf::RHI::ResourceState::RenderTarget, {}},
			{nullptr, depthTexture, depthState, dyf::RHI::ResourceState::DepthWrite, {}}
		}};
		commands->ResourceBarrier(before.data(), depthState == dyf::RHI::ResourceState::DepthWrite ? 1 : 2);
		dyf::RHI::ColorAttachment colorAttachment;
		colorAttachment.texture = target;
		colorAttachment.loadOp = dyf::RHI::LoadOp::Clear;
		colorAttachment.storeOp = dyf::RHI::StoreOp::Store;
		colorAttachment.clearColor[0] = clearColor.x;
		colorAttachment.clearColor[1] = clearColor.y;
		colorAttachment.clearColor[2] = clearColor.z;
		colorAttachment.clearColor[3] = clearColor.w;
		dyf::RHI::DepthStencilAttachment depth;
		depth.texture = depthTexture;
		depth.state = dyf::RHI::ResourceState::DepthWrite;
		depth.depthLoadOp = dyf::RHI::LoadOp::Clear;
		depth.depthStoreOp = dyf::RHI::StoreOp::Discard;
		depth.clearDepth = 1;
		commands->BeginRendering({&colorAttachment, 1, &depth});
		commands->BindGraphicsPipeline(pipeline);
		commands->BindResourceSet(bindings);
		commands->BindVertexBuffer(0, vertices, 0);
		commands->BindIndexBuffer(indices, dyf::RHI::Format::R32_UINT, 0);
		const dyf::RHI::Viewport viewport{0, 0, static_cast<float>(target->GetDesc().width),
			static_cast<float>(target->GetDesc().height), 0, 1};
		const dyf::RHI::Rect scissor{0, 0, target->GetDesc().width, target->GetDesc().height};
		commands->SetViewport(viewport);
		commands->SetScissor(scissor);
		commands->SetInlineConstants(0, sizeof(draw), &draw);
		commands->DrawIndexedInstanced(mesh.indices.size(), 1, 0, 0, 0);
		commands->EndRendering();
		const dyf::RHI::ResourceBarrierDesc present{nullptr, target,
			dyf::RHI::ResourceState::RenderTarget, dyf::RHI::ResourceState::Present, {}};
		commands->ResourceBarrier(&present, 1);
		commands->Close();
		if (!device.Submit(&commands, 1))
		{
			std::fprintf(stderr, "Mesh draw submission failed.\n");
			return 1;
		}
		depthState = dyf::RHI::ResourceState::DepthWrite;
		if (!device.Present()) return 1;
	}
}
