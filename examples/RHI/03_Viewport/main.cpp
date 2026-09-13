#include "dyf.h"
#include "dyf/RHI.h"

#include "vertex.h"
#include "fragment.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace
{
	// 이 예제의 셰이더가 읽는 위치와 색상이다.
	struct Vertex { float x, y, r, g, b, a; };
}

int main()
{
	const uint32_t width = 640, height = 480;
	dyf::Platform::Window window(width, height, "RHI / dyf::RHI::Viewport");
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
	const dyf::RHI::VertexBufferLayout input{0, sizeof(Vertex), dyf::RHI::VertexStepMode::Vertex};
	const std::array<dyf::RHI::VertexAttribute, 2> attributes = {{
		{0, 0, dyf::RHI::Format::R32G32_FLOAT, 0},
		{1, 0, dyf::RHI::Format::R32G32B32A32_FLOAT, 8}
	}};
	const dyf::RHI::ColorAttachmentDesc output{swapchain.format,
		{true, dyf::RHI::BlendFactor::SourceAlpha, dyf::RHI::BlendFactor::OneMinusSourceAlpha, dyf::RHI::BlendOp::Add,
			dyf::RHI::BlendFactor::One, dyf::RHI::BlendFactor::OneMinusSourceAlpha, dyf::RHI::BlendOp::Add}, dyf::RHI::ColorWriteMask::All};
	dyf::RHI::GraphicsPipelineDesc pipelineDesc;
	pipelineDesc.vertexShader = vertexShader;
	pipelineDesc.fragmentShader = fragmentShader;
	pipelineDesc.topology = dyf::RHI::PrimitiveTopology::TriangleList;
	pipelineDesc.vertexBuffers = &input;
	pipelineDesc.vertexBufferCount = 1;
	pipelineDesc.vertexAttributes = attributes.data();
	pipelineDesc.vertexAttributeCount = attributes.size();
	pipelineDesc.raster = {dyf::RHI::FillMode::Solid, dyf::RHI::CullMode::None, dyf::RHI::FrontFace::CounterClockwise, 0, 0, 0};
	pipelineDesc.colorAttachments = &output;
	pipelineDesc.colorAttachmentCount = 1;
	pipelineDesc.layout = {nullptr, 0, 16, dyf::RHI::ShaderStageFlags::Vertex, 15};
	auto* pipeline = resources.Keep(device.CreateGraphicsPipeline(pipelineDesc));
	if (!pipeline)
	{
		std::fprintf(stderr, "RHI pipeline creation failed.\n");
		return 1;
	}

	std::vector<Vertex> vertexData = {
		{20, 20, 0.1f, 0.2f, 0.4f, 1},
		{300, 20, 0.1f, 0.2f, 0.4f, 1},
		{20, 460, 0.1f, 0.2f, 0.4f, 1},
		{20, 460, 0.1f, 0.2f, 0.4f, 1},
		{300, 20, 0.1f, 0.2f, 0.4f, 1},
		{300, 460, 0.1f, 0.2f, 0.4f, 1}
	};

	// 원은 왼쪽 뷰포트 안의 scissor 영역으로 잘라 그린다.
	const uint32_t circleFirst = static_cast<uint32_t>(vertexData.size());
	const dyf::Math::float2 center = {160, 240};
	const float radius = 150;
	const uint32_t segments = 64;
	for (uint32_t i = 0; i < segments; ++i)
	{
		const float angle = i * 6.283185307f / segments;
		const float nextAngle = (i + 1) * 6.283185307f / segments;
		vertexData.push_back({center.x, center.y, 1, 0.4f, 0.2f, 1});
		vertexData.push_back({center.x + radius * std::cos(angle),
			center.y + radius * std::sin(angle), 1, 0.4f, 0.2f, 1});
		vertexData.push_back({center.x + radius * std::cos(nextAngle),
			center.y + radius * std::sin(nextAngle), 1, 0.4f, 0.2f, 1});
	}

	// 오른쪽 사각형과 선은 같은 뷰포트와 scissor를 사용한다.
	const uint32_t rightFirst = static_cast<uint32_t>(vertexData.size());
	vertexData.insert(vertexData.end(), {
		{20, 20, 0.15f, 0.35f, 0.2f, 1},
		{300, 20, 0.15f, 0.35f, 0.2f, 1},
		{20, 460, 0.15f, 0.35f, 0.2f, 1},
		{20, 460, 0.15f, 0.35f, 0.2f, 1},
		{300, 20, 0.15f, 0.35f, 0.2f, 1},
		{300, 460, 0.15f, 0.35f, 0.2f, 1}
	});
	const dyf::Math::float2 start = {0, 0}, end = {320, 480};
	const float lineWidth = 12;
	const float length = std::hypot(end.x - start.x, end.y - start.y);
	const dyf::Math::float2 offset = {-(end.y - start.y) / length * lineWidth / 2,
		(end.x - start.x) / length * lineWidth / 2};
	vertexData.insert(vertexData.end(), {
		{start.x + offset.x, start.y + offset.y, 1, 1, 1, 1},
		{end.x + offset.x, end.y + offset.y, 1, 1, 1, 1},
		{start.x - offset.x, start.y - offset.y, 1, 1, 1, 1},
		{start.x - offset.x, start.y - offset.y, 1, 1, 1, 1},
		{end.x + offset.x, end.y + offset.y, 1, 1, 1, 1},
		{end.x - offset.x, end.y - offset.y, 1, 1, 1, 1}
	});

	// 정점을 올린 뒤 정점 버퍼 상태로 전환한다.
	auto* vertices = resources.Keep(device.CreateBuffer({
		static_cast<uint32_t>(vertexData.size() * sizeof(Vertex)),
		sizeof(Vertex), dyf::RHI::BufferUsage::Vertex, dyf::RHI::ResourceState::CopyDestination}));
	if (!vertices)
	{
		std::fprintf(stderr, "Vertex buffer creation failed.\n");
		return 1;
	}
	dyf::RHI::ResourceScope uploadScope(device);
	auto* upload = uploadScope.Keep(device.AcquireCommandList());
	if (!upload)
	{
		std::fprintf(stderr, "Upload command list unavailable.\n");
		return 1;
	}
	if (!device.UpdateBuffer(*upload, vertices, 0, vertexData.data(), vertices->GetDesc().size))
	{
		std::fprintf(stderr, "Vertex upload failed.\n");
		return 1;
	}
	const dyf::RHI::ResourceBarrierDesc ready{vertices, nullptr,
		dyf::RHI::ResourceState::CopyDestination, dyf::RHI::ResourceState::VertexBuffer, {}};
	upload->ResourceBarrier(&ready, 1);
	upload->Close();
	if (!device.Submit(&upload, 1))
	{
		std::fprintf(stderr, "Upload submission failed.\n");
		return 1;
	}
	const dyf::Math::float4 clearColor = {0.04f, 0.06f, 0.10f, 1};
	while (true)
	{
		window.PollEvents();
		if (!window.IsRunning()) break;
		if (!device.BeginFrame()) continue;

		dyf::RHI::ResourceScope commandsScope(device);
		auto* commands = commandsScope.Keep(device.AcquireCommandList());
		if (!commands)
		{
			std::fprintf(stderr, "Frame command list unavailable.\n");
			return 1;
		}
		auto* target = device.GetBackBuffer();
		const dyf::RHI::ResourceBarrierDesc begin{nullptr, target,
			dyf::RHI::ResourceState::Present, dyf::RHI::ResourceState::RenderTarget, {}};
		commands->ResourceBarrier(&begin, 1);
		dyf::RHI::ColorAttachment colorAttachment;
		colorAttachment.texture = target;
		colorAttachment.loadOp = dyf::RHI::LoadOp::Clear;
		colorAttachment.storeOp = dyf::RHI::StoreOp::Store;
		colorAttachment.clearColor[0] = clearColor.x;
		colorAttachment.clearColor[1] = clearColor.y;
		colorAttachment.clearColor[2] = clearColor.z;
		colorAttachment.clearColor[3] = clearColor.w;
		commands->BeginRendering({&colorAttachment, 1, nullptr});
		commands->BindGraphicsPipeline(pipeline);
		commands->BindVertexBuffer(0, vertices, 0);
		const float transform[4] = {2.f / (width / 2), -2.f / height, -1, 1};
		commands->SetInlineConstants(0, sizeof(transform), transform);

		commands->SetViewport({0, 0, width / 2.f, static_cast<float>(height), 0, 1});
		commands->SetScissor({0, 0, width / 2, height});
		commands->DrawInstanced(circleFirst, 1, 0, 0);

		commands->SetScissor({60, 100, 200, 240});
		commands->DrawInstanced(rightFirst - circleFirst, 1, circleFirst, 0);

		commands->SetViewport({width / 2.f, 0, width / 2.f, static_cast<float>(height), 0, 1});
		commands->SetScissor({static_cast<int32_t>(width / 2), 0, width / 2, height});
		commands->DrawInstanced(static_cast<uint32_t>(vertexData.size()) - rightFirst, 1, rightFirst, 0);
		commands->EndRendering();
		const dyf::RHI::ResourceBarrierDesc end{nullptr, target,
			dyf::RHI::ResourceState::RenderTarget, dyf::RHI::ResourceState::Present, {}};
		commands->ResourceBarrier(&end, 1);
		commands->Close();
		if (!device.Submit(&commands, 1))
		{
			std::fprintf(stderr, "Draw submission failed.\n");
			return 1;
		}
		if (!device.Present()) return 1;
	}
}
