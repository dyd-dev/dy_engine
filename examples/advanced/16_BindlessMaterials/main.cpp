#include "dyf/Platform/Window.h"
#include "dyf/RHI.h"

#include <fstream>
#include <iterator>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace RHI = dyf::RHI;

int main()
{
    const auto readShader = [](const char* path) {
        std::ifstream file(path, std::ios::binary);
        return std::vector<char>{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    };

#ifdef __APPLE__
    const char* vertexEntry="canvasVertex";
    const char* array_fragmentEntry="arrayFragment";
#else
    const char* vertexEntry="main";
    const char* array_fragmentEntry="main";
#endif
    const auto vertexBinary = readShader("Shaders/vertex.bin");
    const auto array_fragmentBinary = readShader("Shaders/array_fragment.bin");

    dyf::Platform::Window window(800, 600, "Advanced / Fixed descriptor arrays");
    std::unique_ptr<RHI::IDevice> owner(RHI::IDevice::Create({}));
    auto& device = *owner;
    RHI::ResourceScope resources(device);
    RHI::SwapchainDesc swapchain;
    swapchain.window = window.GetHandle();
    swapchain.format = RHI::DefaultSwapchainFormat;
    (void)device.CreateSwapchain(swapchain);

    struct Vertex { float x, y, u, v; };
    struct Constants { float scaleX, scaleY, x, y, materialIndex, pad0, pad1, pad2; };
    const std::array<Vertex, 6> vertices{{
        {-0.5f, -0.5f, 0, 0}, {0.5f, -0.5f, 1, 0}, {0.5f, 0.5f, 1, 1},
        {-0.5f, -0.5f, 0, 0}, {0.5f, 0.5f, 1, 1}, {-0.5f, 0.5f, 0, 1}
    }};
    RHI::BufferDesc bufferDesc;
    bufferDesc.size = sizeof(vertices);
    bufferDesc.usage = RHI::BufferUsage::Vertex;
    bufferDesc.initialState = RHI::ResourceState::CopyDestination;
    auto* vertexBuffer = resources.Keep(device.CreateBuffer(bufferDesc));

    constexpr uint32_t materialCount = 4, side = 8;
    const std::array<std::array<uint8_t, 3>, materialCount> palette{{
        {235, 65, 70}, {55, 195, 110}, {60, 130, 235}, {245, 185, 50}
    }};
    std::array<std::array<uint8_t, 4 * 4 * 4>, materialCount> pixels{};
    std::array<RHI::TextureHandle, materialCount> textures{};
    RHI::TextureDesc textureDesc;
    textureDesc.width = textureDesc.height = 4;
    textureDesc.format = RHI::Format::R8G8B8A8_UNORM;
    textureDesc.usage = RHI::TextureUsage::ShaderResource;
    for(uint32_t material = 0; material < materialCount; ++material)
    {
        textures[material] = resources.Keep(device.CreateTexture(textureDesc));
        for(uint32_t y = 0; y < 4; ++y)
            for(uint32_t x = 0; x < 4; ++x)
            {
                auto* pixel = pixels[material].data() + (y * 4 + x) * 4;
                for(uint32_t channel = 0; channel < 3; ++channel)
                    pixel[channel] = (x + y) % 2 == 0 ? palette[material][channel] : palette[material][channel] / 2;
                pixel[3] = 255;
            }
    }

    auto* vertexShader = resources.Keep(device.CreateShader({RHI::ShaderStage::Vertex,
        vertexEntry, vertexBinary.data(), vertexBinary.size()}));
    auto* fragmentShader = resources.Keep(device.CreateShader({RHI::ShaderStage::Fragment,
        array_fragmentEntry, array_fragmentBinary.data(), array_fragmentBinary.size()}));
    RHI::SamplerDesc sampler;
    sampler.minFilter = sampler.magFilter = sampler.mipFilter = RHI::SamplerFilter::Nearest;
    sampler.addressU = sampler.addressV = sampler.addressW = RHI::SamplerAddressMode::ClampToEdge;
    sampler.borderColor = RHI::SamplerBorderColor::TransparentBlack;
    sampler.mipLodBias = sampler.minLod = sampler.maxLod = 0.0f;
    // 배열 크기 4와 드로우마다 동일한 인덱스를 쓰는 고정 디스크립터 배열이다.
    const RHI::ResourceBindingLayout bindings[] = {
        {0, RHI::ResourceBindingType::SampledTexture, materialCount, RHI::ShaderStageFlags::Fragment, {}},
        {4, RHI::ResourceBindingType::StaticSampler, 1, RHI::ShaderStageFlags::Fragment, sampler}
    };
    const RHI::VertexBufferLayout input{0, sizeof(Vertex), RHI::VertexStepMode::Vertex};
    const RHI::VertexAttribute attributes[] = {
        {0, 0, RHI::Format::R32G32_FLOAT, 0}, {1, 0, RHI::Format::R32G32_FLOAT, 8}
    };
    const RHI::ColorAttachmentDesc output{swapchain.format, {}, RHI::ColorWriteMask::All};
    RHI::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.vertexShader = vertexShader;
    pipelineDesc.fragmentShader = fragmentShader;
    pipelineDesc.topology = RHI::PrimitiveTopology::TriangleList;
    pipelineDesc.vertexBuffers = &input;
    pipelineDesc.vertexBufferCount = 1;
    pipelineDesc.vertexAttributes = attributes;
    pipelineDesc.vertexAttributeCount = 2;
    pipelineDesc.raster.fillMode = RHI::FillMode::Solid;
    pipelineDesc.raster.cullMode = RHI::CullMode::None;
    pipelineDesc.raster.frontFace = RHI::FrontFace::CounterClockwise;
    pipelineDesc.colorAttachments = &output;
    pipelineDesc.colorAttachmentCount = 1;
    pipelineDesc.layout = {bindings, 2, sizeof(Constants),
        RHI::ShaderStageFlags::Vertex | RHI::ShaderStageFlags::Fragment, 15};
    auto* pipeline = resources.Keep(device.CreateGraphicsPipeline(pipelineDesc));
    std::array<RHI::ResourceBinding, materialCount> elements{};
    for(uint32_t material = 0; material < materialCount; ++material)
    {
        elements[material].binding = 0;
        elements[material].arrayElement = material;
        elements[material].texture = textures[material];
    }
    auto* materials = resources.Keep(device.CreateResourceSet({pipeline, elements.data(), materialCount}));

    bool uploaded = false;
    while(window.IsRunning())
    {
        window.PollEvents();
        if(!window.IsRunning()) break;
        if(!device.BeginFrame()) continue;
        RHI::ResourceScope frameResources(device);
        auto* commands = frameResources.Keep(device.AcquireCommandList());
        if(!uploaded)
        {
            (void)device.UpdateBuffer(*commands, vertexBuffer, 0, vertices.data(), sizeof(vertices));
            const RHI::ResourceBarrierDesc vertexReady{vertexBuffer, nullptr,
                RHI::ResourceState::CopyDestination, RHI::ResourceState::VertexBuffer, {}};
            commands->ResourceBarrier(&vertexReady, 1);
            for(uint32_t material = 0; material < materialCount; ++material)
            {
                const RHI::ResourceBarrierDesc before{nullptr, textures[material],
                    RHI::ResourceState::Undefined, RHI::ResourceState::CopyDestination, {}};
                commands->ResourceBarrier(&before, 1);
                (void)device.UpdateTexture(*commands, textures[material], 0, 0,
                    pixels[material].data(), uint32_t(pixels[material].size()), 16, 64);
                const RHI::ResourceBarrierDesc after{nullptr, textures[material],
                    RHI::ResourceState::CopyDestination, RHI::ResourceState::ShaderResource, {}};
                commands->ResourceBarrier(&after, 1);
            }
            uploaded = true;
        }
        auto* target = device.GetBackBuffer();
        const auto extent = target->GetDesc();
        const RHI::ResourceBarrierDesc begin{nullptr, target,
            RHI::ResourceState::Present, RHI::ResourceState::RenderTarget, {}};
        commands->ResourceBarrier(&begin, 1);
        RHI::ColorAttachment color;
        color.texture = target;
        color.loadOp = RHI::LoadOp::Clear;
        color.storeOp = RHI::StoreOp::Store;
        color.clearColor[0] = 0.025f;
        color.clearColor[1] = 0.035f;
        color.clearColor[2] = 0.055f;
        color.clearColor[3] = 1.0f;
        commands->BeginRendering({&color, 1, nullptr});
        commands->BindGraphicsPipeline(pipeline);
        commands->BindVertexBuffer(0, vertexBuffer, 0);
        commands->BindResourceSet(materials);
        commands->SetViewport({0, 0, float(extent.width), float(extent.height)});
        commands->SetScissor({0, 0, extent.width, extent.height});
        for(uint32_t i = 0; i < side * side; ++i)
        {
            const Constants constants{
                1.6f / side, 1.6f / side,
                -0.9f + (i % side + 0.5f) * 1.8f / side,
                -0.9f + (i / side + 0.5f) * 1.8f / side,
                float((i + i / side) % materialCount), 0, 0, 0
            };
            commands->SetInlineConstants(0, sizeof(constants), &constants);
            commands->DrawInstanced(uint32_t(vertices.size()), 1, 0, 0);
        }
        commands->EndRendering();
        const RHI::ResourceBarrierDesc end{nullptr, target,
            RHI::ResourceState::RenderTarget, RHI::ResourceState::Present, {}};
        commands->ResourceBarrier(&end, 1);
        (void)commands->Close();
        (void)device.Submit(&commands, 1);
        (void)device.Present();
    }
    (void)device.WaitIdle();
}
