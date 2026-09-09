#include "RHI/ResourceScope.h"
#include <array>
#include <iostream>

namespace
{
using namespace dy::RHI;

// No backend or GPU is needed to verify ownership and the absence of commands.
class Device final : public IDevice
{
public:
    std::vector<const void*> destroyed;
    unsigned operations = 0;
    bool CreateSwapchain(const SwapchainDesc&) override { ++operations; return false; }
    bool BeginFrame() override { ++operations; return false; }
    ICommandList* AcquireCommandList() override { ++operations; return nullptr; }
    bool Submit(ICommandList**, uint32_t) override { ++operations; return false; }
    void Present() override { ++operations; }
    TextureHandle GetBackBuffer() override { ++operations; return nullptr; }
    bool ReadTexture(TextureHandle, TextureReadback&) override { ++operations; return false; }
    BufferHandle CreateBuffer(const BufferDesc&) override { ++operations; return nullptr; }
    TextureHandle CreateTexture(const TextureDesc&) override { ++operations; return nullptr; }
    ShaderHandle CreateShader(const ShaderDesc&) override { ++operations; return nullptr; }
    PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc&) override { ++operations; return nullptr; }
    ResourceSetHandle CreateResourceSet(const ResourceSetDesc&) override { ++operations; return nullptr; }
    void DestroyBuffer(BufferHandle value) override { destroyed.push_back(value); }
    void DestroyTexture(TextureHandle value) override { destroyed.push_back(value); }
    void DestroyShader(ShaderHandle value) override { destroyed.push_back(value); }
    void DestroyPipeline(PipelineHandle value) override { destroyed.push_back(value); }
    void DestroyResourceSet(ResourceSetHandle value) override { destroyed.push_back(value); }
    bool UpdateBuffer(ICommandList&, BufferHandle, uint32_t, const void*, uint32_t) override { ++operations; return false; }
    bool UpdateTexture(ICommandList&, TextureHandle, uint32_t, uint32_t, const void*, uint32_t, uint32_t, uint32_t) override
    { ++operations; return false; }
protected:
    int Initialize(const void*, const DeviceDesc&) override { ++operations; return 0; }
};
}

int main()
{
    Device device;
    std::array<int, 6> tokens{};
    const auto buffer1 = reinterpret_cast<BufferHandle>(&tokens[0]);
    const auto buffer2 = reinterpret_cast<BufferHandle>(&tokens[1]);
    const auto texture = reinterpret_cast<TextureHandle>(&tokens[2]);
    const auto shader = reinterpret_cast<ShaderHandle>(&tokens[3]);
    const auto pipeline = reinterpret_cast<PipelineHandle>(&tokens[4]);
    const auto set = reinterpret_cast<ResourceSetHandle>(&tokens[5]);
    bool rejected = false;
    try
    {
        ResourceScope resources(device);
        resources.Keep(buffer1);
        resources.Keep(buffer2);
        resources.Keep(texture);
        resources.Keep(shader);
        resources.Keep(pipeline);
        resources.Keep(set);
        resources.Keep(BufferHandle{}); // The previous resources must survive until scope exit.
    }
    catch (const std::runtime_error&) { rejected = true; }
    const std::vector<const void*> expected{set, pipeline, shader, texture, buffer2, buffer1};
    if (!rejected || device.operations || device.destroyed != expected)
    {
        std::cerr << "ResourceScope must only release explicitly owned resources, including on failure.\n";
        return 1;
    }
}
