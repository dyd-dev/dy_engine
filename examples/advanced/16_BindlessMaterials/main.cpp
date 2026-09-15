#include "dyf/Platform/Window.h"
#include "dyf/RHI.h"
#include "vertex.h"
#include "fragment.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace RHI = dyf::RHI;
using Clock = std::chrono::steady_clock;

namespace
{
void Check(bool ok, const char* operation)
{
    if (!ok) throw std::runtime_error(operation);
}
struct Options
{
    std::string mode;
    uint32_t frames = 0;
    std::string capture;
    bool validation = false;
};
bool Parse(int argc, char** argv, Options& options, const char* modes, const char* description)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument == "--help")
        {
            std::printf("%s\n--mode %s --frames N (0 = unlimited) --capture image.ppm --validation\n",
                description, modes);
            return false;
        }
        if (argument == "--validation") options.validation = true;
        else if ((argument == "--mode" || argument == "--frames" || argument == "--capture") && i + 1 < argc)
        {
            const std::string value = argv[++i];
            if (argument == "--mode") options.mode = value;
            else if (argument == "--capture") options.capture = value;
            else
            {
                char* end = nullptr;
                const auto count = std::strtoull(value.c_str(), &end, 10);
                Check(!value.empty() && value[0] != '-' && end && *end == '\0' && count <= UINT32_MAX,
                    "--frames requires an unsigned 32-bit integer");
                options.frames = static_cast<uint32_t>(count);
            }
        }
        else throw std::runtime_error("Unknown or incomplete argument; use --help");
    }
    return true;
}
void Capture(RHI::IDevice& device, RHI::TextureHandle texture, const std::string& path)
{
    RHI::TextureReadback image;
    Check(device.ReadTexture(texture, image), "ReadTexture failed (this backend may not support pixel readback)");
    std::ofstream file(path, std::ios::binary);
    Check(bool(file), "Cannot open capture file");
    file << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    const bool bgra = image.format == RHI::Format::B8G8R8A8_UNORM
        || image.format == RHI::Format::B8G8R8A8_UNORM_SRGB;
    for (uint32_t y = 0; y < image.height; ++y)
        for (uint32_t x = 0; x < image.width; ++x)
        {
            const auto* p = image.pixels.data() + size_t(y) * image.rowPitch + size_t(x) * 4;
            const char rgb[3] = {char(p[bgra ? 2 : 0]), char(p[1]), char(p[bgra ? 0 : 2])};
            file.write(rgb, 3);
        }
    Check(bool(file), "Capture write failed");
    std::printf("capture=%s (%ux%u)\n", path.c_str(), image.width, image.height);
}
}

#include "array_fragment.h"

int main(int argc, char** argv)
{
    try
    {
        Options options;
        options.mode = "array";
        if (!Parse(argc, argv, options, "material|array",
            "Fixed arrays, NOT full bindless: four fully populated textures; dynamically uniform material index per draw.")) return 0;
        Check(options.mode == "material" || options.mode == "array", "Invalid --mode");
        std::puts("This example uses a fixed array of 4 sampled textures, NOT full bindless.");
        std::puts("Runtime-sized arrays, non-uniform per-invocation indices, partially bound descriptors and update-after-bind are not exposed by this RHI.");
        dyf::Platform::Window window(800, 600, "Advanced / Fixed descriptor arrays");
        Check(window.GetHandle() != nullptr, "Window creation failed");
        RHI::DeviceDesc deviceDesc;
        deviceDesc.enableValidation = options.validation;
        std::unique_ptr<RHI::IDevice> deviceOwner(RHI::IDevice::Create(deviceDesc));
        Check(bool(deviceOwner), "Device creation failed");
        auto& device = *deviceOwner;
        if (options.mode == "array" && !device.Supports(RHI::Feature::DescriptorIndexing))
        {
            std::puts("Fixed sampled-texture array indexing is unsupported on this device. Run --mode material.");
            return 2;
        }
        RHI::ResourceScope resources(device);
        RHI::SwapchainDesc swapchain;
        swapchain.window = window.GetHandle();
        swapchain.format = RHI::Format::B8G8R8A8_UNORM;
        swapchain.minimumImageCount = 2;
        swapchain.presentMode = RHI::PresentMode::Fifo;
        swapchain.allowReadback = !options.capture.empty();
        if (ShaderData::vertexSize == 0) { swapchain.initialWidth = 800; swapchain.initialHeight = 600; }
        Check(device.CreateSwapchain(swapchain), "Swapchain creation failed");

        struct Vertex { float x, y, u, v; };
        struct Constants { float scaleX, scaleY, x, y, materialIndex, pad0, pad1, pad2; };
        const std::array<Vertex, 6> vertices{{{-0.5f,-0.5f,0,0},{0.5f,-0.5f,1,0},{0.5f,0.5f,1,1},
            {-0.5f,-0.5f,0,0},{0.5f,0.5f,1,1},{-0.5f,0.5f,0,1}}};
        RHI::BufferDesc bufferDesc;
        bufferDesc.size = sizeof(vertices);
        bufferDesc.usage = RHI::BufferUsage::Vertex;
        bufferDesc.initialState = RHI::ResourceState::CopyDestination;
        auto* vertexBuffer = resources.Keep(device.CreateBuffer(bufferDesc));
        constexpr uint32_t materialCount = 4, side = 8, drawCount = side * side;
        const std::array<std::array<uint8_t, 3>, materialCount> palette{{
            {235,65,70},{55,195,110},{60,130,235},{245,185,50}}};
        std::array<std::array<uint8_t, 4 * 4 * 4>, materialCount> pixels{};
        std::array<RHI::TextureHandle, materialCount> textures{};
        RHI::TextureDesc textureDesc;
        textureDesc.width = textureDesc.height = 4;
        textureDesc.format = RHI::Format::R8G8B8A8_UNORM;
        textureDesc.usage = RHI::TextureUsage::ShaderResource;
        for (uint32_t material = 0; material < materialCount; ++material)
        {
            textures[material] = resources.Keep(device.CreateTexture(textureDesc));
            for (uint32_t y = 0; y < 4; ++y)
                for (uint32_t x = 0; x < 4; ++x)
                {
                    auto* p = pixels[material].data() + (y * 4 + x) * 4;
                    const bool bright = (x + y) % 2 == 0;
                    for (uint32_t channel = 0; channel < 3; ++channel)
                        p[channel] = bright ? palette[material][channel] : uint8_t(palette[material][channel] / 2);
                    p[3] = 255;
                }
        }
        auto* vs = resources.Keep(device.CreateShader({RHI::ShaderStage::Vertex,
            ShaderData::vertexEntryPoint, ShaderData::vertex, ShaderData::vertexSize}));
        auto* fs = resources.Keep(device.CreateShader(options.mode == "array"
            ? RHI::ShaderDesc{RHI::ShaderStage::Fragment, ShaderData::array_fragmentEntryPoint,
                ShaderData::array_fragment, ShaderData::array_fragmentSize}
            : RHI::ShaderDesc{RHI::ShaderStage::Fragment, ShaderData::fragmentEntryPoint,
                ShaderData::fragment, ShaderData::fragmentSize}));
        RHI::SamplerDesc sampler;
        sampler.minFilter = sampler.magFilter = sampler.mipFilter = RHI::SamplerFilter::Nearest;
        sampler.addressU = sampler.addressV = sampler.addressW = RHI::SamplerAddressMode::ClampToEdge;
        sampler.borderColor = RHI::SamplerBorderColor::TransparentBlack;
        sampler.mipLodBias = sampler.minLod = sampler.maxLod = 0.0f;
        // 텍스처 배열은 t0..t3에 해당한다. 정적 샘플러는 독립적인 s4/binding 4이다.
        const std::array<RHI::ResourceBindingLayout, 2> bindings{{
            {0, RHI::ResourceBindingType::SampledTexture, options.mode == "array" ? materialCount : 1,
                RHI::ShaderStageFlags::Fragment, {}},
            {4, RHI::ResourceBindingType::StaticSampler, 1, RHI::ShaderStageFlags::Fragment, sampler}}};
        const RHI::VertexBufferLayout input{0, sizeof(Vertex), RHI::VertexStepMode::Vertex};
        const std::array<RHI::VertexAttribute, 2> attributes{{
            {0, 0, RHI::Format::R32G32_FLOAT, 0}, {1, 0, RHI::Format::R32G32_FLOAT, 8}}};
        const RHI::ColorAttachmentDesc output{swapchain.format, {}, RHI::ColorWriteMask::All};
        RHI::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.vertexShader = vs;
        pipelineDesc.fragmentShader = fs;
        pipelineDesc.topology = RHI::PrimitiveTopology::TriangleList;
        pipelineDesc.vertexBuffers = &input;
        pipelineDesc.vertexBufferCount = 1;
        pipelineDesc.vertexAttributes = attributes.data();
        pipelineDesc.vertexAttributeCount = uint32_t(attributes.size());
        pipelineDesc.raster = {RHI::FillMode::Solid, RHI::CullMode::None, RHI::FrontFace::CounterClockwise, 0, 0, 0};
        pipelineDesc.colorAttachments = &output;
        pipelineDesc.colorAttachmentCount = 1;
        pipelineDesc.layout = {bindings.data(), uint32_t(bindings.size()), sizeof(Constants),
            RHI::ShaderStageFlags::Vertex | RHI::ShaderStageFlags::Fragment, 15};
        Check(device.Supports(pipelineDesc), "This device does not support the requested fixed-array/material pipeline");
        auto* pipeline = resources.Keep(device.CreateGraphicsPipeline(pipelineDesc));
        std::array<RHI::ResourceSetHandle, materialCount> sets{};
        if (options.mode == "array")
        {
            std::array<RHI::ResourceBinding, materialCount> elements{};
            for (uint32_t material = 0; material < materialCount; ++material)
            {
                elements[material].binding = 0;
                elements[material].arrayElement = material;
                elements[material].texture = textures[material];
            }
            // 배열의 모든 요소를 채운다. 비어 있는 디스크립터 요소는 허용하지 않는다.
            sets[0] = resources.Keep(device.CreateResourceSet({pipeline, elements.data(), materialCount}));
        }
        else
        {
            for (uint32_t material = 0; material < materialCount; ++material)
            {
                RHI::ResourceBinding element;
                element.binding = 0;
                element.texture = textures[material];
                sets[material] = resources.Keep(device.CreateResourceSet({pipeline, &element, 1}));
            }
        }

        uint32_t frame = 0;
        uint64_t setBinds = 0;
        double recordMilliseconds = 0.0;
        bool captured = false;
        while (window.IsRunning() && (options.frames == 0 || frame < options.frames))
        {
            window.PollEvents();
            if (!window.IsRunning()) break;
            if (!device.BeginFrame())
            {
                Check(!device.IsLost(), "Device lost during BeginFrame");
                continue;
            }
            RHI::ResourceScope frameResources(device);
            auto* commands = frameResources.Keep(device.AcquireCommandList());
            if (frame == 0)
            {
                Check(device.UpdateBuffer(*commands, vertexBuffer, 0, vertices.data(), sizeof(vertices)), "Vertex upload failed");
                const RHI::ResourceBarrierDesc vertexReady{vertexBuffer, nullptr,
                    RHI::ResourceState::CopyDestination, RHI::ResourceState::VertexBuffer, {}};
                commands->ResourceBarrier(&vertexReady, 1);
                for (uint32_t material = 0; material < materialCount; ++material)
                {
                    const RHI::ResourceBarrierDesc before{nullptr, textures[material],
                        RHI::ResourceState::Undefined, RHI::ResourceState::CopyDestination, {}};
                    commands->ResourceBarrier(&before, 1);
                    Check(device.UpdateTexture(*commands, textures[material], 0, 0, pixels[material].data(),
                        uint32_t(pixels[material].size()), 4 * 4, 4 * 4 * 4), "Texture upload failed");
                    const RHI::ResourceBarrierDesc after{nullptr, textures[material],
                        RHI::ResourceState::CopyDestination, RHI::ResourceState::ShaderResource, {}};
                    commands->ResourceBarrier(&after, 1);
                }
            }
            const auto recordStart = Clock::now();
            auto* backBuffer = device.GetBackBuffer();
            const auto& extent = backBuffer->GetDesc();
            const RHI::ResourceBarrierDesc begin{nullptr, backBuffer,
                RHI::ResourceState::Present, RHI::ResourceState::RenderTarget, {}};
            commands->ResourceBarrier(&begin, 1);
            RHI::ColorAttachment color;
            color.texture = backBuffer;
            color.loadOp = RHI::LoadOp::Clear;
            color.storeOp = RHI::StoreOp::Store;
            color.clearColor[0] = 0.025f;
            color.clearColor[1] = 0.035f;
            color.clearColor[2] = 0.055f;
            color.clearColor[3] = 1.0f;
            commands->BeginRendering({&color, 1, nullptr});
            commands->BindGraphicsPipeline(pipeline);
            commands->BindVertexBuffer(0, vertexBuffer, 0);
            commands->SetViewport({0, 0, float(extent.width), float(extent.height), 0, 1});
            commands->SetScissor({0, 0, extent.width, extent.height});
            if (options.mode == "array")
            {
                commands->BindResourceSet(sets[0]);
                ++setBinds;
            }
            for (uint32_t i = 0; i < drawCount; ++i)
            {
                const uint32_t material = (i + i / side) % materialCount;
                if (options.mode == "material")
                {
                    commands->BindResourceSet(sets[material]);
                    ++setBinds;
                }
                // 각 draw 전체에서 같은 index를 사용한다. 픽셀/인스턴스마다 달라지는 비균일 index가 아니다.
                const Constants constants{1.6f / side, 1.6f / side,
                    -0.9f + (i % side + 0.5f) * 1.8f / side, -0.9f + (i / side + 0.5f) * 1.8f / side,
                    float(material), 0, 0, 0};
                commands->SetInlineConstants(0, sizeof(constants), &constants);
                commands->DrawInstanced(uint32_t(vertices.size()), 1, 0, 0);
            }
            commands->EndRendering();
            const RHI::ResourceBarrierDesc end{nullptr, backBuffer,
                RHI::ResourceState::RenderTarget, RHI::ResourceState::Present, {}};
            commands->ResourceBarrier(&end, 1);
            Check(commands->Close(), "Command recording failed");
            recordMilliseconds += std::chrono::duration<double, std::milli>(Clock::now() - recordStart).count();
            Check(device.Submit(&commands, 1), "Submission failed");
            if (!captured && !options.capture.empty() && (options.frames == 0 || frame + 1 == options.frames))
            {
                Capture(device, backBuffer, options.capture);
                captured = true;
            }
            Check(device.Present(), "Present failed");
            ++frame;
        }
        Check(device.WaitIdle(), "WaitIdle failed");
        std::printf("mode=%s frames=%u textures=%u draw_calls=%llu resource_set_binds=%llu cpu_record_ms=%.3f\n",
            options.mode.c_str(), frame, materialCount, static_cast<unsigned long long>(uint64_t(frame) * drawCount),
            static_cast<unsigned long long>(setBinds), recordMilliseconds);
        std::puts("Both modes draw the same 64 quads. Array mode changes a uniform index per draw and binds one set per frame. Timings exclude upload, GPU execution, present and capture.");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
