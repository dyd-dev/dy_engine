#include "dyf/RHI/IDevice.h"
#define private public
#include "Backends/D3D12/D3D12Device.h"
#undef private
#include "Backends/D3D12/D3D12Device.cpp"

#include <array>
#include <stdexcept>
#define P2_SKIP_MSVC_DEBUG_PROXY_FAILURES
#include "P2BackendTestAllocation.h"

namespace
{
using namespace dyf::Backends;
namespace RHI = dyf::RHI;
void Check(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

struct Fixture
{
    D3D12Device device;
    Fixture()
    {
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> warp;
        Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))), "WARP creation failed");
        auto& native = *device.m_internal;
        Check(SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(&native.device))), "D3D12 creation failed");
        D3D12_COMMAND_QUEUE_DESC queue{};
        queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(SUCCEEDED(native.device->CreateCommandQueue(&queue,
            IID_PPV_ARGS(&native.commandQueue))), "Queue creation failed");
        Check(SUCCEEDED(native.device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(&native.fence))), "Fence creation failed");
        native.fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        Check(native.fenceEvent != nullptr, "Event creation failed");
    }
    RHI::BufferHandle Buffer(uint32_t size = 1024)
    {
        RHI::BufferDesc desc{};
        desc.size = size;
        desc.usage = RHI::BufferUsage::Vertex;
        desc.initialState = RHI::ResourceState::CopyDestination;
        auto* buffer = device.CreateBuffer(desc);
        Check(buffer != nullptr, "Buffer creation failed");
        return buffer;
    }
    D3D12CommandList* Commands()
    {
        auto* commands = static_cast<D3D12CommandList*>(device.AcquireCommandListNative());
        Check(commands != nullptr, "Command acquisition failed");
        return commands;
    }
    void Submit(D3D12CommandList* commands)
    {
        Check(commands->CloseNative(), "Close failed");
        RHI::ICommandList* list = commands;
        Check(device.SubmitNative(&list, 1), "Submit failed");
    }
    std::vector<uint8_t> ReadBuffer(D3D12Buffer* buffer)
    {
        auto& native = *device.m_internal;
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        const auto desc = CD3DX12_RESOURCE_DESC::Buffer(buffer->GetDesc().size);
        ComPtr<ID3D12Resource> readback;
        Check(SUCCEEDED(native.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))), "Readback allocation failed");
        auto* commands = Commands();
        auto* list = static_cast<ID3D12GraphicsCommandList*>(commands->GetNativeList());
        auto* source = static_cast<ID3D12Resource*>(buffer->GetNativeResource());
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(source,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &barrier);
        list->CopyBufferRegion(readback.Get(), 0, source, 0, buffer->GetDesc().size);
        barrier = CD3DX12_RESOURCE_BARRIER::Transition(source,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1, &barrier);
        Submit(commands);
        Check(device.WaitIdleNative(), "Buffer readback drain failed");
        std::vector<uint8_t> result(buffer->GetDesc().size);
        void* mapped = nullptr;
        const D3D12_RANGE reads{0, result.size()};
        Check(SUCCEEDED(readback->Map(0, &reads, &mapped)), "Buffer readback map failed");
        std::memcpy(result.data(), mapped, result.size());
        const D3D12_RANGE noWrites{0, 0};
        readback->Unmap(0, &noWrites);
        return result;
    }
};

struct QueueGate
{
    ComPtr<ID3D12Fence> fence;
    explicit QueueGate(Fixture& f)
    {
        Check(SUCCEEDED(f.device.m_internal->device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(&fence))), "Queue gate creation failed");
        Check(SUCCEEDED(f.device.m_internal->commandQueue->Wait(fence.Get(), 1)), "Queue gate wait failed");
    }
    ~QueueGate() { (void)fence->Signal(1); }
};

void RepeatedUploadsAndDiscard()
{
    Fixture f;
    auto* buffer = static_cast<D3D12Buffer*>(f.Buffer());
    std::array<uint8_t, 37> bytes{};
    uint64_t allocations = 0;
    for (unsigned frame = 0; frame < 12; ++frame)
    {
        bytes.fill(uint8_t(frame + 1));
        auto* list = f.Commands();
        for (unsigned update = 0; update < 32; ++update)
            Check(list->RecordBufferUpload(buffer, update, bytes.data(), uint32_t(bytes.size())), "Upload failed");
        const auto recording = f.device.GetUploadMemoryStatistics();
        Check(recording.inUseBytes != 0, "Recording must retain a page");
        if (frame == 0)
        {
            allocations = recording.allocationCount;
            Check(allocations == 1 && recording.requestCount == 32,
                "Multiple small uploads must share one page");
        }
        else Check(recording.allocationCount == allocations, "Repeated uploads must plateau allocations");
        bytes.fill(0);
        if (frame & 1) f.device.DiscardCommandListNative(list);
        else
        {
            f.Submit(list); Check(f.device.WaitIdleNative(), "Drain failed");
            const auto actual = f.ReadBuffer(buffer);
            for (unsigned byte = 0; byte < 32 + bytes.size() - 1; ++byte)
                Check(actual[byte] == frame + 1, "Suballocated buffer offsets or captured bytes changed");
        }
        const auto retired = f.device.GetUploadMemoryStatistics();
        Check(retired.inUseBytes == 0 && retired.cachedBytes != 0, "Retirement/discard must return pages");
    }
    Check(f.device.GetUploadMemoryStatistics().reuseCount >= 11, "Reuse count must grow");
}

void IndependentPolicies()
{
    for(const bool reuse:{false,true})for(const bool suballocate:{false,true})
    {
        Fixture f;
        auto* buffer=static_cast<D3D12Buffer*>(f.Buffer());
        f.device.m_internal->uploadPool=MakeD3D12UploadPool(f.device.m_internal->device.Get());
        f.device.m_internal->uploadPool->SetPolicy({reuse,suballocate});
        for(unsigned frame=0;frame<3;++frame)
        {
            std::array<uint8_t,37> bytes;bytes.fill(uint8_t(frame+10));
            auto* list=f.Commands();
            for(unsigned update=0;update<8;++update)
                Check(list->RecordBufferUpload(buffer,update*32,bytes.data(),uint32_t(bytes.size())),"Policy upload failed");
            bytes.fill(0);f.Submit(list);Check(f.device.WaitIdleNative(),"Policy drain failed");
            const auto actual=f.ReadBuffer(buffer);
            for(unsigned byte=0;byte<7*32+37;++byte)Check(actual[byte]==frame+10,"Policy changed captured buffer payload");
        }
        const uint64_t pages=suballocate?1:8;
        const auto stats=f.device.GetUploadMemoryStatistics();
        Check(stats.requestCount==24 && stats.requestedBytes==24*37,"Policies must preserve requested work");
        Check(stats.allocationCount==pages*(reuse?1:3) && stats.reuseCount==pages*(reuse?2:0),"Policies must independently affect native allocation and reuse");
        Check(stats.inUseBytes==0 && stats.cachedBytes==(reuse?pages*256*1024:0),"Retired policy leases must follow cache setting");
    }
}

void OverlapAndTexturePixels(bool reuse=true,bool suballocate=true)
{
    Fixture f;
    f.device.m_internal->uploadPool=MakeD3D12UploadPool(f.device.m_internal->device.Get());
    f.device.m_internal->uploadPool->SetPolicy({reuse,suballocate});
    auto* buffer = static_cast<D3D12Buffer*>(f.Buffer());
    RHI::TextureDesc desc{};
    desc.width = 3; desc.height = 2;
    desc.format = RHI::Format::R8G8B8A8_UNORM;
    desc.usage = RHI::TextureUsage::ShaderResource;
    auto* texture = static_cast<D3D12Texture*>(f.device.CreateTexture(desc));
    Check(texture != nullptr, "Texture creation failed");
    texture->SetState(0, 0, RHI::ResourceState::Common);
    auto* first = f.Commands();
    auto* second = f.Commands();
    std::array<uint8_t, 37> prefix{};
    Check(first->RecordBufferUpload(buffer, 0, prefix.data(), uint32_t(prefix.size())), "Prefix failed");
    Check(second->RecordBufferUpload(buffer, 64, prefix.data(), uint32_t(prefix.size())), "Concurrent upload failed");
    Check(f.device.GetUploadMemoryStatistics().inUseBytes >= 512 * 1024,
        "Overlapping lists must lease distinct pages");
    RHI::ResourceBarrierDesc barrier{};
    barrier.texture = texture;
    barrier.before = RHI::ResourceState::Common;
    barrier.after = RHI::ResourceState::CopyDestination;
    first->ResourceBarrierNative(&barrier, 1);
    // Source rows have padding; the final row does not need trailing padding.
    std::array<uint8_t, 28> input{};
    for (unsigned row = 0; row < 2; ++row)
        for (unsigned col = 0; col < 12; ++col) input[row * 16 + col] = uint8_t(row * 47 + col);
    const auto expected = input;
    Check(first->RecordTextureUpload(texture, 0, 0, input.data(), uint32_t(input.size()), 16, 32), "Texture upload failed");
    input.fill(0); // Native staging must have captured the bytes already.
    QueueGate gate(f);
    f.Submit(first);
    f.Submit(second);
    // An independent list must not reuse either submitted page while the queue is blocked.
    auto* third = f.Commands();
    const bool uploaded = third->RecordBufferUpload(buffer, 128, prefix.data(), uint32_t(prefix.size()));
    const auto pending = f.device.GetUploadMemoryStatistics();
    f.device.DiscardCommandListNative(third);
    const HRESULT released = gate.fence->Signal(1); // Release the queue before assertions/teardown.
    Check(SUCCEEDED(released), "Queue gate release failed");
    Check(uploaded && pending.inUseBytes >= 768 * 1024,
        "In-flight pages must remain distinct from newly recorded pages");
    Check(f.device.WaitIdleNative(), "Drain failed");
    RHI::TextureReadback pixels;
    Check(f.device.ReadTexture(texture, pixels), "Texture readback failed");
    for (unsigned row = 0; row < 2; ++row)
        for (unsigned col = 0; col < 12; ++col)
            Check(pixels.pixels[row * pixels.rowPitch + col] == expected[row * 16 + col], "Texture pixels changed under upload policy");
}

void FailedSubmitAndOversize()
{
    Fixture f;
    // Record succeeds, but a write without the required state transition fails submit.
    auto* buffer = static_cast<D3D12Buffer*>(f.Buffer());
    buffer->SetState(RHI::ResourceState::Common);
    std::array<uint8_t, 16> bytes{};
    auto* invalid = f.Commands();
    Check(invalid->RecordBufferUpload(buffer, 0, bytes.data(), uint32_t(bytes.size())), "Recording failed");
    Check(invalid->CloseNative(), "Close failed");
    RHI::ICommandList* list = invalid;
    Check(!f.device.SubmitNative(&list, 1), "Invalid resource state must fail submit");
    Check(f.device.GetUploadMemoryStatistics().inUseBytes == 0, "Failed submit must return staging pages");

    constexpr uint32_t largeSize = 9 * 1024 * 1024;
    auto* large = static_cast<D3D12Buffer*>(f.Buffer(largeSize));
    std::vector<uint8_t> largeBytes(largeSize, 71);
    auto* oversized = f.Commands();
    Check(oversized->RecordBufferUpload(large, 0, largeBytes.data(), largeSize), "Oversize upload failed");
    const auto live = f.device.GetUploadMemoryStatistics();
    Check(live.inUseBytes >= largeSize, "Oversize staging span must be leased");
    f.device.DiscardCommandListNative(oversized);
    const auto idle = f.device.GetUploadMemoryStatistics();
    Check(idle.inUseBytes == 0 && idle.cachedBytes <= 8 * 1024 * 1024,
        "Oversize discard must obey idle cache budget");
}

void FailedAllocationAndHostRollback()
{
    Fixture f;
    auto* buffer = static_cast<D3D12Buffer*>(f.Buffer());
    std::array<uint8_t, 64> bytes{};
    // A native allocation/map failure is represented by a failed page factory.
    f.device.m_internal->uploadPool = std::make_shared<D3D12UploadPool>(
        [](uint64_t) -> std::unique_ptr<D3D12UploadPage> { return {}; });
    auto* failed = f.Commands();
    Check(!failed->RecordBufferUpload(buffer, 0, bytes.data(), uint32_t(bytes.size())),
        "Failed page allocation must reject the upload");
    Check(f.device.GetUploadMemoryStatistics().inUseBytes == 0,
        "Failed page allocation must not leak a lease");
    f.device.DiscardCommandListNative(failed);
    f.device.m_internal->uploadPool = MakeD3D12UploadPool(f.device.m_internal->device.Get());
    // Warm the native page so injected failures target C++ bookkeeping rather than the driver.
    auto* warm = f.Commands();
    Check(warm->RecordBufferUpload(buffer, 0, bytes.data(), uint32_t(bytes.size())), "Warmup failed");
    f.device.DiscardCommandListNative(warm);
    unsigned injected = 0;
    for (int after = 0; after < 7; ++after)
    {
        auto* commands = f.Commands();
        bool threw = false;
        P2Allocation::Begin(after);
        try { (void)commands->RecordBufferUpload(buffer, 0, bytes.data(), uint32_t(bytes.size())); }
        catch (const std::bad_alloc&) { threw = true; }
        P2Allocation::End();
        if (threw) ++injected;
        f.device.DiscardCommandListNative(commands);
        Check(f.device.GetUploadMemoryStatistics().inUseBytes == 0,
            "Host allocation failure/discard must return every page");
    }
    Check(injected != 0, "Host failure injection must reach upload bookkeeping");
    auto* recovered = f.Commands();
    Check(recovered->RecordBufferUpload(buffer, 0, bytes.data(), uint32_t(bytes.size())), "Upload retry failed");
    f.Submit(recovered);
    Check(f.device.WaitIdleNative(), "Retry drain failed");
}
}

int main()
{
    try
    {
        RepeatedUploadsAndDiscard();
        IndependentPolicies();
        for(const bool reuse:{false,true})for(const bool suballocate:{false,true})OverlapAndTexturePixels(reuse,suballocate);
        FailedSubmitAndOversize();
        FailedAllocationAndHostRollback();
        std::puts("D3D12 upload page reuse and texture pixels passed");
        return 0;
    }
    catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
