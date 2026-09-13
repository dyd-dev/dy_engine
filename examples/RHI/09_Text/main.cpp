#include "dyf.h"
#include "dyf/RHI.h"

#include "vertex.h"
#include "fragment.h"

#include <array>
#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>
#include <iostream>

// 사용할 폰트 경로는 이 파일에서 지정한다.
#if defined(_WIN32)
#define DY_EXAMPLE_FONT "C:/Windows/Fonts/malgun.ttf"
#elif defined(__APPLE__)
#define DY_EXAMPLE_FONT "/System/Library/Fonts/Helvetica.ttc"
#else
#define DY_EXAMPLE_FONT "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
#endif

namespace
{
	// 이 예제의 셰이더가 읽는 위치·UV·색상이다.
	struct CanvasVertex { float x,y,u,v,r,g,b,a; };
}

int main()
{
	constexpr uint32_t windowWidth = 640, windowHeight = 480;
	dyf::Platform::Window window(windowWidth, windowHeight, "RHI / Text");
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
	const dyf::RHI::VertexBufferLayout vertexLayout{0, sizeof(CanvasVertex), dyf::RHI::VertexStepMode::Vertex};
	const std::array<dyf::RHI::VertexAttribute, 3> attributes = {{
		{0, 0, dyf::RHI::Format::R32G32_FLOAT, 0},
		{1, 0, dyf::RHI::Format::R32G32_FLOAT, 8},
		{2, 0, dyf::RHI::Format::R32G32B32A32_FLOAT, 16}
	}};
	dyf::RHI::SamplerDesc sampler;
	sampler.minFilter = dyf::RHI::SamplerFilter::Linear;
	sampler.magFilter = dyf::RHI::SamplerFilter::Linear;
	sampler.mipFilter = dyf::RHI::SamplerFilter::Linear;
	sampler.addressU = dyf::RHI::SamplerAddressMode::ClampToEdge;
	sampler.addressV = dyf::RHI::SamplerAddressMode::ClampToEdge;
	sampler.addressW = dyf::RHI::SamplerAddressMode::ClampToEdge;
	sampler.minLod = 0;
	sampler.maxLod = 0;
	sampler.mipLodBias = 0;
	const std::array<dyf::RHI::ResourceBindingLayout, 2> bindingLayout = {{
		{0, dyf::RHI::ResourceBindingType::SampledTexture, 1, dyf::RHI::ShaderStageFlags::Fragment, {}},
		{1, dyf::RHI::ResourceBindingType::StaticSampler, 1, dyf::RHI::ShaderStageFlags::Fragment, sampler}
	}};
	const dyf::RHI::ColorAttachmentDesc colorOutput{
		swapchain.format,
		{true, dyf::RHI::BlendFactor::SourceAlpha, dyf::RHI::BlendFactor::OneMinusSourceAlpha, dyf::RHI::BlendOp::Add,
			dyf::RHI::BlendFactor::One, dyf::RHI::BlendFactor::OneMinusSourceAlpha, dyf::RHI::BlendOp::Add},
		dyf::RHI::ColorWriteMask::All
	};
	dyf::RHI::GraphicsPipelineDesc pipelineDesc;
	pipelineDesc.vertexShader = vertexShader;
	pipelineDesc.fragmentShader = fragmentShader;
	pipelineDesc.topology = dyf::RHI::PrimitiveTopology::TriangleList;
	pipelineDesc.vertexBuffers = &vertexLayout;
	pipelineDesc.vertexBufferCount = 1;
	pipelineDesc.vertexAttributes = attributes.data();
	pipelineDesc.vertexAttributeCount = attributes.size();
	pipelineDesc.raster = {dyf::RHI::FillMode::Solid, dyf::RHI::CullMode::None, dyf::RHI::FrontFace::CounterClockwise, 0, 0, 0};
	pipelineDesc.colorAttachments = &colorOutput;
	pipelineDesc.colorAttachmentCount = 1;
	pipelineDesc.layout = {bindingLayout.data(), static_cast<uint32_t>(bindingLayout.size()), 16, dyf::RHI::ShaderStageFlags::Vertex, 15};
	auto* pipeline = resources.Keep(device.CreateGraphicsPipeline(pipelineDesc));
	if(!pipeline)
	{
		std::cerr << "RHI pipeline creation failed." << '\n';
		return 1;
	}
	auto font = dyf::Font::Load(DY_EXAMPLE_FONT, 32, 1024, 1024);
	if(!font)
	{
		std::cerr << "Font loading failed." << '\n';
		return 1;
	}
	std::vector<CanvasVertex> vertexData;
	uint32_t clippedFirst = 0;
	for(uint32_t textIndex = 0; textIndex < 3; ++textIndex)
	{
		std::string_view text = "Renderer = RHI commands\nText, kerning and alpha blending";
		dyf::Math::float2 position = {30, 70};
		dyf::Math::float4 color = {1, 1, 1, 1};
		if(textIndex == 1)
		{
			text = "0123456789  AVATAR";
			position = {30, 190};
			color = {0.3f, 0.8f, 1, 1};
		}
		else if(textIndex == 2)
		{
			text = "Clipped text across the viewport";
			position = {30, 270};
			color = {1, 0.7f, 0.2f, 0.75f};
			clippedFirst = static_cast<uint32_t>(vertexData.size());
		}

		float penX = position.x, penY = position.y + font->GetAscent();
		uint32_t previousCodepoint = 0;
		for(size_t byteOffset = 0; byteOffset < text.size();)
		{
			const auto codepoint = dyf::DecodeUtf8(text, byteOffset);
			if(codepoint == '\n')
			{
				penX = position.x;
				penY += font->GetLineHeight();
				previousCodepoint = 0;
				continue;
			}
			if(previousCodepoint) penX += font->GetKerning(previousCodepoint, codepoint);
			dyf::FontGlyph glyph;
			if(!font->GetGlyph(codepoint, glyph))
			{
				std::cerr << "Text layout failed.\n";
				return 1;
			}
			if(glyph.width && glyph.height)
			{
				const float left = penX + glyph.offsetX, top = penY + glyph.offsetY;
				const float right = left + glyph.width, bottom = top + glyph.height;
				const float uvLeft = static_cast<float>(glyph.x), uvTop = static_cast<float>(glyph.y);
				const float uvRight = uvLeft + glyph.width, uvBottom = uvTop + glyph.height;
				vertexData.insert(vertexData.end(), {
					{left, top, uvLeft, uvTop, color.x, color.y, color.z, color.w},
					{right, top, uvRight, uvTop, color.x, color.y, color.z, color.w},
					{left, bottom, uvLeft, uvBottom, color.x, color.y, color.z, color.w},
					{left, bottom, uvLeft, uvBottom, color.x, color.y, color.z, color.w},
					{right, top, uvRight, uvTop, color.x, color.y, color.z, color.w},
					{right, bottom, uvRight, uvBottom, color.x, color.y, color.z, color.w}
				});
			}
			penX += glyph.advance;
			previousCodepoint = codepoint;
		}
	}

	// 모든 글리프를 준비한 뒤 픽셀을 공유하고 아틀라스 크기로 UV를 정규화한다.
	const dyf::Image image = font->GetAtlas();
	for(auto& vertex : vertexData)
	{
		vertex.u /= static_cast<float>(image.GetWidth());
		vertex.v /= static_cast<float>(image.GetHeight());
	}
	// 글꼴 아틀라스와 정점을 RHI 자원으로 생성하고 업로드한다.
	auto* vertices = resources.Keep(device.CreateBuffer({
		static_cast<uint32_t>(vertexData.size() * sizeof(CanvasVertex)), sizeof(CanvasVertex),
		dyf::RHI::BufferUsage::Vertex, dyf::RHI::ResourceState::CopyDestination
	}));
	dyf::RHI::TextureDesc textureDesc;
	textureDesc.width = image.GetWidth();
	textureDesc.height = image.GetHeight();
	textureDesc.mipLevels = 1;
	textureDesc.depthOrArraySize = 1;
	textureDesc.format = image.GetColorSpace() == dyf::ColorSpace::Srgb
		? dyf::RHI::Format::R8G8B8A8_UNORM_SRGB : dyf::RHI::Format::R8G8B8A8_UNORM;
	textureDesc.usage = dyf::RHI::TextureUsage::ShaderResource;
	auto* texture = resources.Keep(device.CreateTexture(textureDesc));
	if(!vertices || !texture)
	{
		std::cerr << "Resource creation failed." << '\n';
		return 1;
	}
	dyf::RHI::ResourceScope uploadScope(device);
	auto* upload = uploadScope.Keep(device.AcquireCommandList());
	if(!upload)
	{
		std::cerr << "Upload command list unavailable." << '\n';
		return 1;
	}
	dyf::RHI::ResourceBarrierDesc textureBarrier{nullptr, texture, dyf::RHI::ResourceState::Undefined, dyf::RHI::ResourceState::CopyDestination, {}};
	upload->ResourceBarrier(&textureBarrier, 1);
	if(!device.UpdateBuffer(*upload, vertices, 0, vertexData.data(), vertices->GetDesc().size) ||
		!device.UpdateTexture(*upload, texture, 0, 0,
			image.GetPixels().data(), static_cast<uint32_t>(image.GetPixels().size()),
			image.GetWidth() * 4, image.GetWidth() * image.GetHeight() * 4))
	{
		std::cerr << "Upload failed.\n";
		return 1;
	}
	const std::array<dyf::RHI::ResourceBarrierDesc, 2> ready = {{
		{vertices, nullptr, dyf::RHI::ResourceState::CopyDestination, dyf::RHI::ResourceState::VertexBuffer, {}},
		{nullptr, texture, dyf::RHI::ResourceState::CopyDestination, dyf::RHI::ResourceState::ShaderResource, {}}
	}};
	upload->ResourceBarrier(ready.data(), ready.size());
	if(!upload->Close() || !device.Submit(&upload, 1))
	{
		std::cerr << "Upload failed." << '\n';
		return 1;
	}
	dyf::RHI::ResourceBinding binding;
	binding.binding = 0;
	binding.texture = texture;
	auto* bindings = resources.Keep(device.CreateResourceSet({pipeline, &binding, 1}));
	if(!bindings)
	{
		std::cerr << "Resource bindings failed." << '\n';
		return 1;
	}

	while(true)
	{
		window.PollEvents();
		if(!window.IsRunning()) break;

		if(!device.BeginFrame()) continue;
		dyf::RHI::ResourceScope commandsScope(device);
		auto* commands = commandsScope.Keep(device.AcquireCommandList());
		if(!commands)
		{
			std::cerr << "Frame command list unavailable." << '\n';
			return 1;
		}
		auto* backBuffer = device.GetBackBuffer();
		const dyf::RHI::ResourceBarrierDesc begin{nullptr, backBuffer, dyf::RHI::ResourceState::Present, dyf::RHI::ResourceState::RenderTarget, {}};
		commands->ResourceBarrier(&begin, 1);
		dyf::RHI::ColorAttachment color;
		color.texture = backBuffer;
		color.loadOp = dyf::RHI::LoadOp::Clear;
		color.storeOp = dyf::RHI::StoreOp::Store;
		color.clearColor[0] = 0.04f;
		color.clearColor[1] = 0.06f;
		color.clearColor[2] = 0.10f;
		color.clearColor[3] = 1;
		commands->BeginRendering({&color, 1, nullptr});
		commands->BindGraphicsPipeline(pipeline);
		commands->BindResourceSet(bindings);
		commands->BindVertexBuffer(0, vertices, 0);
		commands->SetViewport({0, 0, static_cast<float>(windowWidth), static_cast<float>(windowHeight), 0, 1});
		const float transform[4] = {2.f / windowWidth, -2.f / windowHeight, -1, 1};
		commands->SetInlineConstants(0, sizeof(transform), transform);
		commands->SetScissor({0, 0, windowWidth, windowHeight});
		commands->DrawInstanced(clippedFirst, 1, 0, 0);

		commands->SetScissor({60, 260, 480, 60});
		commands->DrawInstanced(static_cast<uint32_t>(vertexData.size()) - clippedFirst, 1, clippedFirst, 0);
		commands->EndRendering();
		const dyf::RHI::ResourceBarrierDesc end{nullptr, backBuffer, dyf::RHI::ResourceState::RenderTarget, dyf::RHI::ResourceState::Present, {}};
		commands->ResourceBarrier(&end, 1);
		if(!commands->Close() || !device.Submit(&commands, 1))
		{
			std::cerr << "Draw submission failed." << '\n';
			return 1;
		}
		if(!device.Present())
		{
			std::cerr << "Presentation failed.\n";
			return 1;
		}
	}
	return 0;
}
