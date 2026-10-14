#include <dyf/RHI.h>
#include <dyf/Platform/Log.h>
#include "Backends/Null/NullDevice.h"
#define P2_SKIP_MSVC_DEBUG_PROXY_FAILURES
#include "P2BackendTestAllocation.h"
#undef P2_SKIP_MSVC_DEBUG_PROXY_FAILURES
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace RHI = dyf::RHI;
namespace
{
int failures = 0;
void Check(bool value, const char* message)
{
    if(!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
int factoryMode = 0, factoryLive = 0;
RHI::IDevice* factorySafetyOwner = nullptr;
struct InitializeFailure {};
}
namespace dyf::Backends
{
// Exercise the production factory with a native initialization boundary that can
// return failure or throw. The safety owner makes baseline failure tests bounded.
class P2FactoryDevice : public NullDevice
{
public:
    P2FactoryDevice() { ++factoryLive; factorySafetyOwner = this; }
    ~P2FactoryDevice() override { --factoryLive; factorySafetyOwner = nullptr; }
protected:
    int Initialize(const void* handle, const RHI::DeviceDesc& desc) override
    {
        if(factoryMode == 1) throw InitializeFailure{};
        if(factoryMode == 2) return -1;
        return NullDevice::Initialize(handle, desc);
    }
};
}
#define NullDevice P2FactoryDevice
#include "../src/RHI/IDevice.cpp"
#undef NullDevice

namespace
{
class TestPipeline final : public RHI::Pipeline
{
public:
    TestPipeline(const RHI::PipelineLayoutDesc& layout, bool compute) : Pipeline(layout, compute) {}
    ~TestPipeline() override = default;
};
class Device final : public dyf::Backends::NullDevice
{
public:
    Device() { Check(Initialize(nullptr, {}) == 0, "Null initialization failed"); }
    ~Device() override
    {
        ReleaseResources();
        // Do not leak a baseline native object after reporting failed ownership.
        for(auto*& pipeline : nativePipelines) { delete pipeline; pipeline = nullptr; }
    }
    bool SupportsNative(RHI::Feature feature) const override
    {
        return feature == RHI::Feature::Compute || NullDevice::SupportsNative(feature);
    }
    RHI::PipelineHandle CreateGraphicsPipelineNative(const RHI::GraphicsPipelineDesc& desc) override
    { return MakePipeline(desc.layout, false); }
    RHI::PipelineHandle CreateComputePipelineNative(const RHI::ComputePipelineDesc& desc) override
    { return MakePipeline(desc.layout, true); }
    void DestroyPipelineNative(RHI::PipelineHandle handle) override
    {
        for(auto*& pipeline : nativePipelines)
            if(pipeline == handle) { delete pipeline; pipeline = nullptr; --nativeLive; return; }
        Check(false, "native pipeline destroyed twice or without ownership");
    }
    RHI::ICommandList* AcquireCommandListNative() override
    { ++nativeAcquisitions; return NullDevice::AcquireCommandListNative(); }
    RHI::TextureHandle GetBackBufferNative() override
    { return stableBackBuffer ? stableBackBuffer : NullDevice::GetBackBufferNative(); }
    bool BeginFrameNative() override { return stableBackBuffer ? true : NullDevice::BeginFrameNative(); }
    bool PresentNative() override { return stableBackBuffer ? true : NullDevice::PresentNative(); }
    void MakeStableBackBuffer()
    {
        RHI::SwapchainDesc desc; desc.initialWidth = desc.initialHeight = 4;
        desc.format = RHI::Format::R8G8B8A8_UNORM;
        Check(CreateSwapchain(desc) && NullDevice::BeginFrameNative(), "test swapchain creation failed");
        // Native owner retains this real Null backbuffer until device destruction,
        // so stale-generation rejection is tested without any dangling access.
        stableBackBuffer = NullDevice::GetBackBufferNative();
    }
    int failAfterNative = -1, nativeLive = 0, nativeAcquisitions = 0;
private:
    RHI::PipelineHandle MakePipeline(const RHI::PipelineLayoutDesc& layout, bool compute)
    {
        auto pipeline = std::make_unique<TestPipeline>(layout, compute);
        for(auto*& slot : nativePipelines)
            if(!slot)
            {
                slot = pipeline.release(); ++nativeLive;
                P2Allocation::failAfter = failAfterNative;
                return slot;
            }
        throw std::runtime_error("test pipeline storage exhausted");
    }
    std::array<TestPipeline*, 16> nativePipelines{};
    RHI::TextureHandle stableBackBuffer = nullptr;
};
RHI::ShaderHandle Shader(Device& device, RHI::ShaderStage stage)
{
    static const unsigned char binary[] = {1, 2, 3, 4};
    return device.CreateShader({stage, "main", binary, sizeof(binary)});
}

void FactoryFailureOwnsDevice()
{
    factoryMode = 1;
    bool threw = false;
    try { (void)RHI::IDevice::Create({}); } catch(const InitializeFailure&) { threw = true; }
    Check(threw, "factory must preserve initialization exception");
    Check(factoryLive == 0, "factory must destroy device after initialization exception");
    if(factorySafetyOwner) delete factorySafetyOwner;
    factoryMode = 2;
    Check(RHI::IDevice::Create({}) == nullptr && factoryLive == 0, "ordinary native failure must return null and destroy device");
    factoryMode = 0;
    { std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create({})); Check(device && factoryLive == 1, "successful factory transfers ownership"); }
    Check(factoryLive == 0, "successful factory result must remain normally destructible");
}

void RecordedListRegistrationOwnsObject()
{
    for(int allocation = 0; allocation < 6; ++allocation)
    {
        auto device = std::make_unique<Device>();
        P2Allocation::Begin(allocation);
        try
        {
            auto* list = device->AcquireCommandList();
            device->DestroyCommandList(list);
        }
        catch(const std::bad_alloc&) {}
        P2Allocation::failAfter = -1;
        device.reset();
        const unsigned remaining = P2Allocation::End();
        Check(remaining == 0, "recorded-list registration failure left unowned allocation");
    }
}

void PipelineRegistrationOwnsNative(bool compute)
{
    for(int allocation = 0; allocation < 5; ++allocation)
    {
        Device device;
        auto* shader = Shader(device, compute ? RHI::ShaderStage::Compute : RHI::ShaderStage::Vertex);
        Check(shader != nullptr, "test shader creation failed");
        RHI::GraphicsPipelineDesc graphics;
        graphics.vertexShader = shader;
        graphics.fragmentShader = compute ? nullptr : Shader(device, RHI::ShaderStage::Fragment);
        graphics.topology = RHI::PrimitiveTopology::TriangleList;
        graphics.raster = {RHI::FillMode::Solid, RHI::CullMode::None, RHI::FrontFace::CounterClockwise};
        const RHI::VertexBufferLayout input{0, 8, RHI::VertexStepMode::Vertex};
        const RHI::VertexAttribute attribute{0, 0, RHI::Format::R32G32_FLOAT, 0};
        const RHI::ColorAttachmentDesc color{RHI::Format::R8G8B8A8_UNORM, {}, RHI::ColorWriteMask::All};
        graphics.vertexBuffers = &input; graphics.vertexBufferCount = 1;
        graphics.vertexAttributes = &attribute; graphics.vertexAttributeCount = 1;
        graphics.colorAttachments = &color; graphics.colorAttachmentCount = 1;
        if(!compute) Check(device.Supports(graphics), "test graphics description rejected");
        device.failAfterNative = allocation;
        RHI::PipelineHandle pipeline = nullptr;
        try { pipeline = compute ? device.CreateComputePipeline({shader, {}}) : device.CreateGraphicsPipeline(graphics); }
        catch(const std::bad_alloc&) {}
        P2Allocation::failAfter = -1;
        if(pipeline) device.DestroyPipeline(pipeline);
        Check(device.nativeLive == 0, "pipeline metadata/registration failure retained a native pipeline");
        Check(device.GetResourceAllocationCounters().pipelines.live == 0, "failed pipeline creation changed live counters");
        device.DestroyShader(shader);
        if(graphics.fragmentShader) device.DestroyShader(graphics.fragmentShader);
    }
}

void BackBufferGenerationCheckedBeforeReplay(bool initiallyPrepared)
{
    Device device;
    device.MakeStableBackBuffer();
    Check(device.BeginFrame(), "first frame failed");
    auto* texture = device.GetBackBuffer();
    auto* list = device.AcquireCommandList();
    const RHI::ResourceBarrierDesc barrier{nullptr, texture, RHI::ResourceState::Present, RHI::ResourceState::Present, {}};
    list->ResourceBarrier(&barrier, 1);
    Check(list->Close(), "backbuffer list close failed");
    if(initiallyPrepared) Check(device.PrepareCommandLists(&list, 1), "current-generation list must prepare");
    Check(device.Present(), "first present failed");
    const int before = device.nativeAcquisitions;
    Check(!device.PrepareCommandLists(&list, 1), "backbuffer list must be rejected outside an active frame");
    Check(device.nativeAcquisitions == before, "inactive-frame list allocated native work before rejection");
    Check(device.BeginFrame(), "second frame failed");
    Check(!device.PrepareCommandLists(&list, 1), "old-generation list must be rejected");
    Check(device.nativeAcquisitions == before, "old-generation list allocated native work before rejection");
    device.DestroyCommandList(list);
    auto* empty = device.AcquireCommandList();
    Check(empty->Close() && device.PrepareCommandLists(&empty, 1), "offscreen-only list must still prepare");
    Check(device.PrepareCommandLists(&empty, 1), "repeated preparation must remain valid");
    device.DestroyCommandList(empty);
    Check(device.Present(), "second present failed");
}
}

int main()
{
    dyf::Platform::Log::SetDefaultOutputEnabled(false);
    FactoryFailureOwnsDevice();
    RecordedListRegistrationOwnsObject();
    PipelineRegistrationOwnsNative(false);
    PipelineRegistrationOwnsNative(true);
    BackBufferGenerationCheckedBeforeReplay(false);
    BackBufferGenerationCheckedBeforeReplay(true);
    std::printf("P2 RHI checks: %d failures\n", failures);
    return failures ? 1 : 0;
}
