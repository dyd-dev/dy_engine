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
	dyf::Platform::Window window(width, height, "RHI / Primitives");
	if(!window.GetHandle()) return 1;

	std::unique_ptr<dyf::RHI::IDevice> deviceOwner(dyf::RHI::IDevice::Create(dyf::RHI::DeviceDesc{}));
	if(!deviceOwner)
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
	if(!device.CreateSwapchain(swapchain))
	{
		std::fprintf(stderr, "Swapchain creation failed.\n");
		return 1;
	}

	// 도형을 그릴 순서대로 삼각형 정점을 만든다.
	std::vector<Vertex> vertexData;
	const std::array<dyf::Math::float2, 6> corners = {{{0, 0}, {1, 0}, {0, 1}, {0, 1}, {1, 0}, {1, 1}}};

	// 점은 지정한 크기의 사각형이다.
	dyf::Math::float4 color = {1, 0.7f, 0.1f, 1};
	const dyf::Math::float2 position = {70, 70};
	const float pointSize = 10;
	dyf::Rectangle rectangle = {position.x - pointSize / 2, position.y - pointSize / 2, pointSize, pointSize};
	for(const auto& corner : corners)
	{
		vertexData.push_back({rectangle.x + corner.x * rectangle.width,
			rectangle.y + corner.y * rectangle.height, color.x, color.y, color.z, color.w});
	}

	// 두께가 있는 선은 선분에 수직인 방향으로 펼친 삼각형 두 개다.
	dyf::Math::float2 start = {100, 70}, end = {260, 130};
	color = {0.1f, 0.8f, 1, 1};
	float lineWidth = 6;
	float dx = end.x - start.x, dy = end.y - start.y;
	float length = std::hypot(dx, dy);
	dyf::Math::float2 offset = {-dy / length * lineWidth / 2, dx / length * lineWidth / 2};
	for(const auto& point : {dyf::Math::float2{start.x + offset.x, start.y + offset.y},
		{end.x + offset.x, end.y + offset.y}, {start.x - offset.x, start.y - offset.y},
		{start.x - offset.x, start.y - offset.y}, {end.x + offset.x, end.y + offset.y},
		{end.x - offset.x, end.y - offset.y}})
	{
		vertexData.push_back({point.x, point.y, color.x, color.y, color.z, color.w});
	}

	// 사각형의 네 변에도 같은 두께 계산을 적용한다.
	rectangle = {320, 45, 200, 100};
	color = {1, 0.3f, 0.4f, 1};
	lineWidth = 5;
	const std::array<dyf::Math::float2, 4> outline = {{
		{rectangle.x, rectangle.y}, {rectangle.x + rectangle.width, rectangle.y},
		{rectangle.x + rectangle.width, rectangle.y + rectangle.height}, {rectangle.x, rectangle.y + rectangle.height}
	}};
	for(uint32_t i = 0; i < outline.size(); ++i)
	{
		start = outline[i];
		end = outline[(i + 1) % outline.size()];
		dx = end.x - start.x;
		dy = end.y - start.y;
		length = std::hypot(dx, dy);
		offset = {-dy / length * lineWidth / 2, dx / length * lineWidth / 2};
		for(const auto& point : {dyf::Math::float2{start.x + offset.x, start.y + offset.y},
			{end.x + offset.x, end.y + offset.y}, {start.x - offset.x, start.y - offset.y},
			{start.x - offset.x, start.y - offset.y}, {end.x + offset.x, end.y + offset.y},
			{end.x - offset.x, end.y - offset.y}})
		{
			vertexData.push_back({point.x, point.y, color.x, color.y, color.z, color.w});
		}
	}

	rectangle = {45, 210, 180, 120};
	color = {0.2f, 0.8f, 0.4f, 1};
	for(const auto& corner : corners)
	{
		vertexData.push_back({rectangle.x + corner.x * rectangle.width,
			rectangle.y + corner.y * rectangle.height, color.x, color.y, color.z, color.w});
	}

	// 원의 테두리는 두께가 있는 선분 64개로 만든다.
	dyf::Math::float2 center = {330, 275};
	float radius = 60;
	color = {0.8f, 0.5f, 1, 1};
	lineWidth = 4;
	const uint32_t segments = 64;
	for(uint32_t i = 0; i < segments; ++i)
	{
		const float angle = i * 6.283185307f / segments;
		const float nextAngle = (i + 1) * 6.283185307f / segments;
		start = {center.x + radius * std::cos(angle), center.y + radius * std::sin(angle)};
		end = {center.x + radius * std::cos(nextAngle), center.y + radius * std::sin(nextAngle)};
		dx = end.x - start.x;
		dy = end.y - start.y;
		length = std::hypot(dx, dy);
		offset = {-dy / length * lineWidth / 2, dx / length * lineWidth / 2};
		for(const auto& point : {dyf::Math::float2{start.x + offset.x, start.y + offset.y},
			{end.x + offset.x, end.y + offset.y}, {start.x - offset.x, start.y - offset.y},
			{start.x - offset.x, start.y - offset.y}, {end.x + offset.x, end.y + offset.y},
			{end.x - offset.x, end.y - offset.y}})
		{
			vertexData.push_back({point.x, point.y, color.x, color.y, color.z, color.w});
		}
	}

	// 채운 원은 중심과 둘레를 잇는 삼각형 64개다.
	center = {510, 300};
	radius = 70;
	color = {1, 0.65f, 0.15f, 1};
	for(uint32_t i = 0; i < segments; ++i)
	{
		const float angle = i * 6.283185307f / segments;
		const float nextAngle = (i + 1) * 6.283185307f / segments;
		start = {center.x + radius * std::cos(angle), center.y + radius * std::sin(angle)};
		end = {center.x + radius * std::cos(nextAngle), center.y + radius * std::sin(nextAngle)};
		for(const auto& point : {center, start, end})
		{
			vertexData.push_back({point.x, point.y, color.x, color.y, color.z, color.w});
		}
	}

	// 위치와 색상만 입력받는 삼각형 파이프라인을 만든다.
	auto* vertexShader = resources.Keep(device.CreateShader({dyf::RHI::ShaderStage::Vertex,
		ShaderData::vertexEntryPoint, ShaderData::vertex, ShaderData::vertexSize}));
	auto* fragmentShader = resources.Keep(device.CreateShader({dyf::RHI::ShaderStage::Fragment,
		ShaderData::fragmentEntryPoint, ShaderData::fragment, ShaderData::fragmentSize}));
	if(!vertexShader || !fragmentShader)
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
	if(!pipeline)
	{
		std::fprintf(stderr, "RHI pipeline creation failed.\n");
		return 1;
	}

	// 정점을 한 번 올린 뒤 그리기에 사용할 상태로 전환한다.
	auto* vertices = resources.Keep(device.CreateBuffer({
		static_cast<uint32_t>(vertexData.size() * sizeof(Vertex)),
		sizeof(Vertex), dyf::RHI::BufferUsage::Vertex, dyf::RHI::ResourceState::CopyDestination}));
	if(!vertices)
	{
		std::fprintf(stderr, "Vertex buffer creation failed.\n");
		return 1;
	}
	dyf::RHI::ResourceScope uploadScope(device);
	auto* upload = uploadScope.Keep(device.AcquireCommandList());
	if(!upload)
	{
		std::fprintf(stderr, "Upload command list unavailable.\n");
		return 1;
	}
	if(!device.UpdateBuffer(*upload, vertices, 0, vertexData.data(), vertices->GetDesc().size))
	{
		std::fprintf(stderr, "Vertex upload failed.\n");
		return 1;
	}
	const dyf::RHI::ResourceBarrierDesc ready{vertices, nullptr,
		dyf::RHI::ResourceState::CopyDestination, dyf::RHI::ResourceState::VertexBuffer, {}};
	upload->ResourceBarrier(&ready, 1);
	if(!upload->Close() || !device.Submit(&upload, 1))
	{
		std::fprintf(stderr, "Upload submission failed.\n");
		return 1;
	}

	const dyf::RHI::Viewport viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
	const dyf::RHI::Rect scissor{0, 0, width, height};
	const float transform[4] = {2 / viewport.width, -2 / viewport.height, -1, 1};
	const dyf::Math::float4 clearColor = {0.04f, 0.06f, 0.10f, 1};
	while(true)
	{
		window.PollEvents();
		if(!window.IsRunning()) break;
		if(!device.BeginFrame()) continue;

		dyf::RHI::ResourceScope commandsScope(device);
		auto* commands = commandsScope.Keep(device.AcquireCommandList());
		if(!commands)
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
		commands->SetViewport(viewport);
		commands->SetScissor(scissor);
		commands->SetInlineConstants(0, sizeof(transform), transform);
		commands->DrawInstanced(static_cast<uint32_t>(vertexData.size()), 1, 0, 0);
		commands->EndRendering();
		const dyf::RHI::ResourceBarrierDesc end{nullptr, target,
			dyf::RHI::ResourceState::RenderTarget, dyf::RHI::ResourceState::Present, {}};
		commands->ResourceBarrier(&end, 1);
		if(!commands->Close() || !device.Submit(&commands, 1))
		{
			std::fprintf(stderr, "Draw submission failed.\n");
			return 1;
		}
		if(!device.Present()) return 1;
	}
}
