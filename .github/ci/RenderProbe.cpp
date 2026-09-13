#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "Platform/Window.h"
#include "RHI/IBuffer.h"
#include "RHI/ICommandList.h"
#include "RHI/IDevice.h"
#include "RHI/IPipelineState.h"
#include "RHI/ITexture.h"

#ifndef DY_CI_SHADER_DIR
#define DY_CI_SHADER_DIR "./Shaders"
#endif

namespace
{
using namespace dy;
constexpr uint32_t width = 640, height = 480;
#if defined(ENABLE_VULKAN)
constexpr const char* backend = "vulkan";
constexpr const char* shaderExtension = ".spv";
#elif defined(ENABLE_D3D12)
constexpr const char* backend = "d3d12";
constexpr const char* shaderExtension = ".hlsl";
#elif defined(ENABLE_METAL)
constexpr const char* backend = "metal";
constexpr const char* shaderExtension = ".metal";
#else
constexpr const char* backend = "null";
constexpr const char* shaderExtension = "";
#endif

void Require(bool condition, const char* message)
{
    if(!condition) throw std::runtime_error(message);
}

std::vector<char> ReadShader(const char* name)
{
    const std::string path = std::string(DY_CI_SHADER_DIR) + "/" + name + shaderExtension;
    std::ifstream stream(path, std::ios::binary);
    Require(stream.good(), ("Cannot open fixture shader: " + path).c_str());
    std::vector<char> bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    Require(!bytes.empty(), "Fixture shader is empty.");
    bytes.push_back('\0'); // Metal consumes a terminated source string; size excludes this byte.
    return bytes;
}

// Matches the RHI's D3D input layout; GLSL/MSL read the same packed float data.
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
    float parameters[4] = {1, 0.5f, 1, 0}; // scale, depth, clip-space Y sign, textured
    float unused[44] = {}; // Public default ShaderLayoutDesc reserves 208 bytes.
};
static_assert(sizeof(Constants) == 208);

struct Resources
{
    RHI::IDevice* device;
    RHI::IBuffer* vertices = nullptr;
    RHI::ITexture* texture = nullptr;
    RHI::ITexture* depth = nullptr;
    RHI::IPipelineState* pipeline = nullptr;
    void Destroy()
    {
        device->DestroyTexture(depth); depth = nullptr;
        device->DestroyTexture(texture); texture = nullptr;
        device->DestroyPipelineState(pipeline); pipeline = nullptr;
        device->DestroyBuffer(vertices); vertices = nullptr;
    }
    ~Resources() { Destroy(); }
};

void CheckCounter(const RHI::ResourceAllocationCounter& before,
                  const RHI::ResourceAllocationCounter& after, uint64_t created, uint64_t destroyed)
{
    Require(after.created - before.created == created, "Unexpected RHI resource creation count.");
    Require(after.destroyed - before.destroyed == destroyed, "Unexpected RHI resource destruction count.");
    Require(after.live + destroyed == before.live + created, "Unexpected live RHI resource count.");
}
}

int main(int argc, char** argv)
{
    try
    {
        Require(argc == 3 && std::string(argv[1]) == "--case", "Usage: CiRenderProbe --case triangle|depth|texture");
        const std::string scenario = argv[2];
        Require(scenario == "triangle" || scenario == "depth" || scenario == "texture", "Unknown GPU fixture case.");
        Require(std::string(backend) != "null", "GPU fixture requires a real graphics backend.");
        std::cout << "[CI fixture] name=render-probe backend=" << backend << " case=" << scenario << std::endl;
        const auto vs = ReadShader("probe_vs"), ps = ReadShader("probe_ps");
        Platform::Window window(width, height, "CI Render Probe");
        std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create(window.GetHandle()));
        Require(device != nullptr, "Cannot create RHI device.");
        const auto baseline = device->GetResourceAllocationCounters();
        {
            Resources resources{device.get()};
            // Counterclockwise in clip space. The texture quad uses the same winding.
            const std::vector<Vertex> vertices = scenario == "texture"
                ? std::vector<Vertex>{{{-0.8f,-0.8f,0},{0,0,1},{0,1}}, {{0.8f,-0.8f,0},{0,0,1},{1,1}}, {{0.8f,0.8f,0},{0,0,1},{1,0}},
                                      {{-0.8f,-0.8f,0},{0,0,1},{0,1}}, {{0.8f,0.8f,0},{0,0,1},{1,0}}, {{-0.8f,0.8f,0},{0,0,1},{0,0}}}
                : std::vector<Vertex>{{{-0.8f,-0.8f,0}}, {{0.8f,-0.8f,0}}, {{0,0.8f,0}}};
            RHI::BufferDesc bufferDesc{};
            bufferDesc.size = static_cast<uint32_t>(vertices.size() * sizeof(Vertex));
            bufferDesc.stride = sizeof(Vertex);
            bufferDesc.usage = RHI::BufferUsage::Vertex | RHI::BufferUsage::Storage;
            resources.vertices = device->CreateBuffer(bufferDesc);
            Require(resources.vertices != nullptr, "Cannot create fixture vertex buffer.");
            void* mapped = resources.vertices->Map(0);
            Require(mapped != nullptr, "Cannot map fixture vertex buffer.");
            std::memcpy(mapped, vertices.data(), bufferDesc.size);
            resources.vertices->Unmap();

            RHI::TextureDesc textureDesc{};
            textureDesc.width = textureDesc.height = 16;
            textureDesc.format = RHI::Format::R8G8B8A8_UNORM;
            textureDesc.usage = RHI::TextureUsage::ShaderResource;
            resources.texture = device->CreateTexture(textureDesc);
            Require(resources.texture != nullptr, "Cannot create fixture color texture.");
            std::array<uint8_t, 16 * 16 * 4> pixels{};
            constexpr uint8_t colors[4][4] = {{255,0,0,255}, {0,255,0,255}, {0,0,255,255}, {255,255,255,255}};
            for(uint32_t y = 0; y < 16; ++y)
                for(uint32_t x = 0; x < 16; ++x)
                    std::memcpy(pixels.data() + (y * 16 + x) * 4, colors[(y >= 8 ? 2 : 0) + (x >= 8 ? 1 : 0)], 4);
            Require(device->UpdateTexture(resources.texture, pixels.data(), 16 * 4), "Cannot upload fixture texture.");
            const auto textureSlot = device->AllocateDescriptorSlot();
            Require(textureSlot != RHI::INVALID_DESCRIPTOR_INDEX, "Cannot allocate fixture texture descriptor.");
            device->UpdateDescriptorSlot(textureSlot, resources.texture);
            RHI::TextureDesc depthDesc{};
            depthDesc.width = width; depthDesc.height = height;
            depthDesc.format = RHI::Format::D32_FLOAT;
            depthDesc.usage = RHI::TextureUsage::DepthStencil;
            resources.depth = device->CreateTexture(depthDesc);
            Require(resources.depth != nullptr, "Cannot create fixture depth texture.");

            RHI::GraphicsPipelineDesc pipelineDesc{};
            pipelineDesc.vertexShader = vs.data(); pipelineDesc.vertexShaderSize = vs.size() - 1;
            pipelineDesc.pixelShader = ps.data(); pipelineDesc.pixelShaderSize = ps.size() - 1;
            pipelineDesc.renderTargetFormat = device->GetDesc().swapchainFormat;
            pipelineDesc.depthStencilFormat = depthDesc.format;
            pipelineDesc.depthEnable = scenario == "depth";
            pipelineDesc.blendEnable = false;
            resources.pipeline = device->CreateGraphicsPipeline(pipelineDesc);
            Require(resources.pipeline != nullptr, "Cannot create fixture graphics pipeline.");
            uint64_t frames = 0;
            auto frame = [&](bool draw)
            {
                device->BeginFrame();
                auto* target = device->GetBackBuffer();
                auto* commands = device->AcquireCommandList();
                Require(target != nullptr && commands != nullptr, "RHI frame resources are unavailable.");
                commands->SetRenderTargets(1, &target, draw ? resources.depth : nullptr);
                commands->ClearColor(target, 0, 0, 1, 1);
                if(draw)
                {
                    commands->ClearDepth(resources.depth, 1);
                    commands->SetViewport({0, 0, float(width), float(height), 0, 1});
                    commands->SetScissor({0, 0, width, height});
                    commands->BindGraphicsPipeline(resources.pipeline);
                    commands->BindGlobalDescriptors();
                    commands->BindGeometry({resources.vertices, sizeof(Vertex)});
                    commands->BindTexture(device->GetDesc().shaderLayout.baseColorTextureBinding, resources.texture);
                    Constants constants;
                    constants.parameters[2] = device->RequiresClipSpaceYFlip() ? -1.0f : 1.0f;
                    if(scenario == "depth")
                    {
                        constants.color[0] = 0; constants.color[1] = 1;
                        constants.parameters[0] = 0.6f; constants.parameters[1] = 0.2f;
                        commands->SetInlineConstants(sizeof(constants), &constants);
                        commands->DrawInstanced(3, 1, 0, 0); // Near green FIRST; farther red must not overwrite it.
                        constants.color[0] = 1; constants.color[1] = 0;
                        constants.parameters[0] = 1; constants.parameters[1] = 0.8f;
                    }
                    constants.parameters[3] = scenario == "texture" ? 1.0f : 0.0f;
                    commands->SetInlineConstants(sizeof(constants), &constants);
                    commands->DrawInstanced(static_cast<uint32_t>(vertices.size()), 1, 0, 0);
                }
                commands->Close();
                device->Submit(&commands, 1);
                device->Present();
            };
            while(window.IsRunning())
            {
                window.PollEvents();
                if(!window.IsRunning()) break;
                frame(true);
                ++frames;
            }
            Require(frames != 0, "No fixture frame was submitted.");
            // Present cycles the public frame fences. Stop referencing fixture objects before destroying them.
            for(uint32_t i = 0; i <= device->GetDesc().maxFramesInFlight; ++i) frame(false);
            const auto allocated = device->GetResourceAllocationCounters();
            CheckCounter(baseline.buffers, allocated.buffers, 1, 0);
            CheckCounter(baseline.textures, allocated.textures, 2, 0);
            CheckCounter(baseline.pipelines, allocated.pipelines, 1, 0);
            resources.Destroy();
            const auto released = device->GetResourceAllocationCounters();
            CheckCounter(baseline.buffers, released.buffers, 1, 1);
            CheckCounter(baseline.textures, released.textures, 2, 2);
            CheckCounter(baseline.pipelines, released.pipelines, 1, 1);
            std::cout << "[CI fixture] resource_balance=ok submitted_frames=" << frames << std::endl;
        }
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
