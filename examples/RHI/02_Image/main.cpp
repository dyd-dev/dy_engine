#include "dyf.h"
#include "dyf/RHI.h"

#include "vertex.h"
#include "fragment.h"

#include <array>
#include <cstdio>
#include <memory>

#define DY_EXAMPLE_IMAGE "Assets/Default_albedo.jpg"

namespace
{
	// 이 예제의 셰이더가 읽는 위치·UV·색상이다.
	struct CanvasVertex { float x,y,u,v,r,g,b,a; };
}

int main()
{
	const uint32_t width = 640, height = 480;
	dyf::Platform::Window window(width, height, "RHI / Image");
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
	const dyf::RHI::VertexBufferLayout input{0, sizeof(CanvasVertex), dyf::RHI::VertexStepMode::Vertex};
	const std::array<dyf::RHI::VertexAttribute, 3> attributes = {{
		{0, 0, dyf::RHI::Format::R32G32_FLOAT, 0},
		{1, 0, dyf::RHI::Format::R32G32_FLOAT, 8},
		{2, 0, dyf::RHI::Format::R32G32B32A32_FLOAT, 16}
	}};
	dyf::RHI::SamplerDesc sampler;
	sampler.minFilter = sampler.magFilter = sampler.mipFilter = dyf::RHI::SamplerFilter::Linear;
	sampler.addressU = sampler.addressV = sampler.addressW = dyf::RHI::SamplerAddressMode::ClampToEdge;
	sampler.minLod = sampler.maxLod = sampler.mipLodBias = 0;
	const std::array<dyf::RHI::ResourceBindingLayout, 2> bindingLayout = {{
		{0, dyf::RHI::ResourceBindingType::SampledTexture, 1, dyf::RHI::ShaderStageFlags::Fragment, {}},
		{1, dyf::RHI::ResourceBindingType::StaticSampler, 1, dyf::RHI::ShaderStageFlags::Fragment, sampler}
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
	pipelineDesc.layout = {bindingLayout.data(), static_cast<uint32_t>(bindingLayout.size()),
		16, dyf::RHI::ShaderStageFlags::Vertex, 15};
	auto* pipeline = resources.Keep(device.CreateGraphicsPipeline(pipelineDesc));
	if (!pipeline)
	{
		std::fprintf(stderr, "RHI pipeline creation failed.\n");
		return 1;
	}

	dyf::Image image;
	if (!dyf::LoadImage(DY_EXAMPLE_IMAGE, image)) return 1;

	// 같은 이미지와 뷰포트를 사용하는 두 사각형을 한 번에 그린다.
	const std::array<CanvasVertex, 12> vertexData = {{
		{40, 70, 0, 0, 1, 1, 1, 1},
		{280, 70, 1, 0, 1, 1, 1, 1},
		{40, 310, 0, 1, 1, 1, 1, 1},
		{40, 310, 0, 1, 1, 1, 1, 1},
		{280, 70, 1, 0, 1, 1, 1, 1},
		{280, 310, 1, 1, 1, 1, 1, 1},
		{340, 110, 0, 0, 1, 0.5f, 0.4f, 0.65f},
		{580, 110, 1, 0, 1, 0.5f, 0.4f, 0.65f},
		{340, 350, 0, 1, 1, 0.5f, 0.4f, 0.65f},
		{340, 350, 0, 1, 1, 0.5f, 0.4f, 0.65f},
		{580, 110, 1, 0, 1, 0.5f, 0.4f, 0.65f},
		{580, 350, 1, 1, 1, 0.5f, 0.4f, 0.65f}
	}};

	// 정점과 픽셀을 올린 뒤, 그리기에 사용할 자원 상태로 전환한다.
	auto* vertices = resources.Keep(device.CreateBuffer({
		static_cast<uint32_t>(vertexData.size() * sizeof(CanvasVertex)),
		sizeof(CanvasVertex), dyf::RHI::BufferUsage::Vertex, dyf::RHI::ResourceState::CopyDestination}));
	dyf::RHI::TextureDesc textureDesc;
	textureDesc.width = image.GetWidth();
	textureDesc.height = image.GetHeight();
	textureDesc.depthOrArraySize = textureDesc.mipLevels = 1;
	textureDesc.format = image.GetColorSpace() == dyf::ColorSpace::Srgb
		? dyf::RHI::Format::R8G8B8A8_UNORM_SRGB : dyf::RHI::Format::R8G8B8A8_UNORM;
	textureDesc.usage = dyf::RHI::TextureUsage::ShaderResource;
	auto* texture = resources.Keep(device.CreateTexture(textureDesc));
	if (!vertices || !texture)
	{
		std::fprintf(stderr, "Resource creation failed.\n");
		return 1;
	}
	dyf::RHI::ResourceScope uploadScope(device);
	auto* upload = uploadScope.Keep(device.AcquireCommandList());
	if (!upload)
	{
		std::fprintf(stderr, "Upload command list unavailable.\n");
		return 1;
	}
	const dyf::RHI::ResourceBarrierDesc textureBarrier{nullptr, texture,
		dyf::RHI::ResourceState::Undefined, dyf::RHI::ResourceState::CopyDestination, {}};
	upload->ResourceBarrier(&textureBarrier, 1);
	if (!device.UpdateBuffer(*upload, vertices, 0, vertexData.data(), vertices->GetDesc().size))
	{
		std::fprintf(stderr, "Vertex upload failed.\n");
		return 1;
	}
	if (!device.UpdateTexture(*upload, texture, 0, 0, image.GetPixels().data(),
		static_cast<uint32_t>(image.GetPixels().size()), image.GetWidth() * 4, image.GetWidth() * image.GetHeight() * 4))
	{
		std::fprintf(stderr, "Texture upload failed.\n");
		return 1;
	}
	const std::array<dyf::RHI::ResourceBarrierDesc, 2> ready = {{
		{vertices, nullptr, dyf::RHI::ResourceState::CopyDestination, dyf::RHI::ResourceState::VertexBuffer, {}},
		{nullptr, texture, dyf::RHI::ResourceState::CopyDestination, dyf::RHI::ResourceState::ShaderResource, {}}
	}};
	upload->ResourceBarrier(ready.data(), ready.size());
	upload->Close();
	if (!device.Submit(&upload, 1))
	{
		std::fprintf(stderr, "Upload submission failed.\n");
		return 1;
	}
	dyf::RHI::ResourceBinding binding;
	binding.binding = 0;
	binding.texture = texture;
	auto* bindings = resources.Keep(device.CreateResourceSet({pipeline, &binding, 1}));
	if (!bindings)
	{
		std::fprintf(stderr, "Resource bindings failed.\n");
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
		commands->BindResourceSet(bindings);
		commands->BindVertexBuffer(0, vertices, 0);
		commands->SetViewport({0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1});
		commands->SetScissor({0, 0, width, height});
		const float transform[4] = {2.f / width, -2.f / height, -1, 1};
		commands->SetInlineConstants(0, sizeof(transform), transform);
		commands->DrawInstanced(static_cast<uint32_t>(vertexData.size()), 1, 0, 0);
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
