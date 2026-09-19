#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "dyf/Platform/Window.h"
#include "dyf/RHI.h"
#include "probe_vertex.h"
#include "probe_fragment.h"

#ifndef DY_CI_BACKEND
#error DY_CI_BACKEND must name the configured graphics backend.
#endif

namespace
{
using namespace dyf;
constexpr uint32_t width = 640, height = 480;

void Require(bool condition, const char* message)
{
    if(!condition) throw std::runtime_error(message);
}

struct Vertex
{
    float position[3];
    float normal[3] = {0, 0, 1};
    float uv[2] = {};
    float tangent[4] = {1, 0, 0, 1};
};
static_assert(sizeof(Vertex) == 48);

struct Constants
{
    float color[4] = {1, 0, 0, 1};
    float parameters[4] = {1, 0.5f, 1, 0}; // scale, depth, reserved, textured
};
static_assert(sizeof(Constants) == 32);
}

int main(int argc, char** argv)
{
    try
    {
        Require(argc == 3 && std::string(argv[1]) == "--case", "Usage: CiRenderProbe --case triangle|depth|texture");
        const std::string scenario = argv[2];
        Require(scenario == "triangle" || scenario == "depth" || scenario == "texture", "Unknown GPU fixture case.");
        Require(std::string(DY_CI_BACKEND) != "null", "GPU fixture requires a real graphics backend.");
        std::cout << "[CI fixture] name=render-probe backend=" << DY_CI_BACKEND << " case=" << scenario << std::endl;
        Platform::Window window(width, height, "CI Render Probe");
        Require(window.GetHandle() != nullptr, "Cannot create fixture window.");
        std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create({}));
        Require(device != nullptr, "Cannot create RHI device.");
        RHI::SwapchainDesc swapchain;
        swapchain.window = window.GetHandle();
        swapchain.format = RHI::Format::B8G8R8A8_UNORM;
        Require(device->CreateSwapchain(swapchain), "Cannot create fixture swapchain.");
        uint64_t frames = 0;
        {
            RHI::ResourceScope resources(*device);
            const auto vertexShader = resources.Keep(device->CreateShader({RHI::ShaderStage::Vertex,
                ShaderData::probe_vertexEntryPoint, ShaderData::probe_vertex, ShaderData::probe_vertexSize}));
            const auto fragmentShader = resources.Keep(device->CreateShader({RHI::ShaderStage::Fragment,
                ShaderData::probe_fragmentEntryPoint, ShaderData::probe_fragment, ShaderData::probe_fragmentSize}));
            // Counterclockwise in clip space. The texture quad uses the same winding.
            const std::vector<Vertex> vertices = scenario == "texture"
                ? std::vector<Vertex>{{{-0.8f,-0.8f,0},{0,0,1},{0,1}}, {{0.8f,-0.8f,0},{0,0,1},{1,1}}, {{0.8f,0.8f,0},{0,0,1},{1,0}},
                                      {{-0.8f,-0.8f,0},{0,0,1},{0,1}}, {{0.8f,0.8f,0},{0,0,1},{1,0}}, {{-0.8f,0.8f,0},{0,0,1},{0,0}}}
                : std::vector<Vertex>{{{-0.8f,-0.8f,0}}, {{0.8f,-0.8f,0}}, {{0,0.8f,0}}};
            const auto vertexBuffer = resources.Keep(device->CreateBuffer({
                static_cast<uint32_t>(vertices.size() * sizeof(Vertex)), sizeof(Vertex),
                RHI::BufferUsage::Vertex | RHI::BufferUsage::Storage, RHI::ResourceState::CopyDestination}));

            RHI::TextureDesc textureDesc;
            textureDesc.width = textureDesc.height = 16;
            textureDesc.format = RHI::Format::R8G8B8A8_UNORM;
            textureDesc.usage = RHI::TextureUsage::ShaderResource;
            const auto texture = resources.Keep(device->CreateTexture(textureDesc));
            std::array<uint8_t, 16 * 16 * 4> pixels{};
            constexpr uint8_t colors[4][4] = {{255,0,0,255}, {0,255,0,255}, {0,0,255,255}, {255,255,255,255}};
            for(uint32_t y = 0; y < 16; ++y)
                for(uint32_t x = 0; x < 16; ++x)
                    std::memcpy(pixels.data() + (y * 16 + x) * 4, colors[(y >= 8 ? 2 : 0) + (x >= 8 ? 1 : 0)], 4);
            {
                RHI::ResourceScope uploadScope(*device);
                auto* upload = uploadScope.Keep(device->AcquireCommandList());
                const RHI::ResourceBarrierDesc textureUpload{nullptr, texture,
                    RHI::ResourceState::Undefined, RHI::ResourceState::CopyDestination, {}};
                upload->ResourceBarrier(&textureUpload, 1);
                Require(device->UpdateBuffer(*upload, vertexBuffer, 0, vertices.data(), vertexBuffer->GetDesc().size),
                        "Cannot upload fixture vertex buffer.");
                Require(device->UpdateTexture(*upload, texture, 0, 0, pixels.data(),
                            static_cast<uint32_t>(pixels.size()), 16 * 4, static_cast<uint32_t>(pixels.size())),
                        "Cannot upload fixture texture.");
                const std::array<RHI::ResourceBarrierDesc, 2> ready = {{
                    {vertexBuffer, nullptr, RHI::ResourceState::CopyDestination, RHI::ResourceState::VertexBuffer, {}},
                    {nullptr, texture, RHI::ResourceState::CopyDestination, RHI::ResourceState::ShaderResource, {}}
                }};
                upload->ResourceBarrier(ready.data(), static_cast<uint32_t>(ready.size()));
                Require(upload->Close(), "Fixture upload recording failed.");
                Require(device->Submit(&upload, 1), "Fixture upload submission failed.");
                Require(device->WaitIdle(), "Fixture upload completion failed.");
            }

            const auto* backBuffer = device->GetBackBuffer();
            Require(backBuffer != nullptr, "Fixture backbuffer is unavailable.");
            RHI::TextureDesc depthDesc;
            depthDesc.width = backBuffer->GetDesc().width;
            depthDesc.height = backBuffer->GetDesc().height;
            depthDesc.format = RHI::Format::D32_FLOAT;
            depthDesc.usage = RHI::TextureUsage::DepthStencil;
            const auto depth = resources.Keep(device->CreateTexture(depthDesc));
            RHI::ResourceState depthState = RHI::ResourceState::Undefined;

            const RHI::VertexBufferLayout input{0, sizeof(Vertex), RHI::VertexStepMode::Vertex};
            const std::array<RHI::VertexAttribute, 2> attributes = {{
                {0, 0, RHI::Format::R32G32B32_FLOAT, offsetof(Vertex, position)},
                {1, 0, RHI::Format::R32G32_FLOAT, offsetof(Vertex, uv)}
            }};
            RHI::SamplerDesc sampler;
            sampler.minFilter = sampler.magFilter = sampler.mipFilter = RHI::SamplerFilter::Linear;
            sampler.addressU = sampler.addressV = sampler.addressW = RHI::SamplerAddressMode::ClampToEdge;
            sampler.minLod = sampler.maxLod = sampler.mipLodBias = 0;
            const std::array<RHI::ResourceBindingLayout, 2> layout = {{
                {0, RHI::ResourceBindingType::SampledTexture, 1, RHI::ShaderStageFlags::Fragment, {}},
                {1, RHI::ResourceBindingType::StaticSampler, 1, RHI::ShaderStageFlags::Fragment, sampler}
            }};
            const RHI::ColorAttachmentDesc output{swapchain.format, {}, RHI::ColorWriteMask::All};
            RHI::GraphicsPipelineDesc pipelineDesc;
            pipelineDesc.vertexShader = vertexShader;
            pipelineDesc.fragmentShader = fragmentShader;
            pipelineDesc.topology = RHI::PrimitiveTopology::TriangleList;
            pipelineDesc.vertexBuffers = &input;
            pipelineDesc.vertexBufferCount = 1;
            pipelineDesc.vertexAttributes = attributes.data();
            pipelineDesc.vertexAttributeCount = static_cast<uint32_t>(attributes.size());
            pipelineDesc.raster = {RHI::FillMode::Solid, RHI::CullMode::Back, RHI::FrontFace::CounterClockwise, 0, 0, 0};
            pipelineDesc.depthStencil.format = depthDesc.format;
            pipelineDesc.depthStencil.depthTestEnabled = pipelineDesc.depthStencil.depthWriteEnabled = scenario == "depth";
            pipelineDesc.depthStencil.depthCompareOp = RHI::CompareOp::Less;
            pipelineDesc.colorAttachments = &output;
            pipelineDesc.colorAttachmentCount = 1;
            pipelineDesc.layout = {layout.data(), static_cast<uint32_t>(layout.size()), sizeof(Constants),
                RHI::ShaderStageFlags::Vertex | RHI::ShaderStageFlags::Fragment, 15};
            const auto pipeline = resources.Keep(device->CreateGraphicsPipeline(pipelineDesc));
            const RHI::ResourceBinding binding{0, 0, nullptr, texture, 0, 0, {}};
            const auto bindings = resources.Keep(device->CreateResourceSet({pipeline, &binding, 1}));

            while(window.IsRunning())
            {
                window.PollEvents();
                if(!window.IsRunning()) break;
                if(!device->BeginFrame())
                {
                    Require(!device->IsLost(), "RHI device lost while acquiring a frame.");
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                RHI::ResourceScope commandsScope(*device);
                auto* target = device->GetBackBuffer();
                auto* commands = commandsScope.Keep(device->AcquireCommandList());
                Require(target != nullptr, "RHI backbuffer is unavailable.");
                Require(target->GetDesc().width == depthDesc.width && target->GetDesc().height == depthDesc.height,
                        "Fixture window extent changed during observation.");
                const std::array<RHI::ResourceBarrierDesc, 2> begin = {{
                    {nullptr, target, RHI::ResourceState::Present, RHI::ResourceState::RenderTarget, {}},
                    {nullptr, depth, depthState, RHI::ResourceState::DepthWrite, {}}
                }};
                commands->ResourceBarrier(begin.data(), depthState == RHI::ResourceState::DepthWrite ? 1 : 2);
                RHI::ColorAttachment colorAttachment;
                colorAttachment.texture = target;
                colorAttachment.loadOp = RHI::LoadOp::Clear;
                colorAttachment.storeOp = RHI::StoreOp::Store;
                colorAttachment.clearColor[2] = colorAttachment.clearColor[3] = 1;
                RHI::DepthStencilAttachment depthAttachment;
                depthAttachment.texture = depth;
                depthAttachment.state = RHI::ResourceState::DepthWrite;
                depthAttachment.depthLoadOp = RHI::LoadOp::Clear;
                depthAttachment.depthStoreOp = RHI::StoreOp::Discard;
                depthAttachment.clearDepth = 1;
                commands->BeginRendering({&colorAttachment, 1, &depthAttachment});
                commands->SetViewport({0, 0, float(depthDesc.width), float(depthDesc.height), 0, 1});
                commands->SetScissor({0, 0, depthDesc.width, depthDesc.height});
                commands->BindGraphicsPipeline(pipeline);
                commands->BindResourceSet(bindings);
                commands->BindVertexBuffer(0, vertexBuffer, 0);
                Constants constants;
                if(scenario == "depth")
                {
                    constants.color[0] = 0; constants.color[1] = 1;
                    constants.parameters[0] = 0.6f; constants.parameters[1] = 0.2f;
                    commands->SetInlineConstants(0, sizeof(constants), &constants);
                    commands->DrawInstanced(3, 1, 0, 0); // Near green FIRST; farther red must not overwrite it.
                    constants.color[0] = 1; constants.color[1] = 0;
                    constants.parameters[0] = 1; constants.parameters[1] = 0.8f;
                }
                constants.parameters[3] = scenario == "texture" ? 1.0f : 0.0f;
                commands->SetInlineConstants(0, sizeof(constants), &constants);
                commands->DrawInstanced(static_cast<uint32_t>(vertices.size()), 1, 0, 0);
                commands->EndRendering();
                const RHI::ResourceBarrierDesc present{nullptr, target,
                    RHI::ResourceState::RenderTarget, RHI::ResourceState::Present, {}};
                commands->ResourceBarrier(&present, 1);
                Require(commands->Close(), "Fixture frame recording failed.");
                Require(device->Submit(&commands, 1), "Fixture frame submission failed.");
                depthState = RHI::ResourceState::DepthWrite;
                Require(device->Present(), "Fixture frame presentation failed.");
                ++frames;
            }
            Require(frames != 0, "No fixture frame was submitted.");
            Require(device->WaitIdle() && !device->IsLost(), "Fixture resource completion failed.");
        }
        // ResourceScope released every owned handle after completion; allocation counters are not public.
        std::cout << "[CI fixture] resource_lifecycle=ok submitted_frames=" << frames << std::endl;
        device.reset();
        std::cout << "[CI fixture] shutdown=ok" << std::endl;
        return 0;
    }
    catch(const std::exception& error)
    {
        std::cerr << "[CI fixture] failure=" << error.what() << std::endl;
        return 1;
    }
}
