#include "dyf/RHI/IDevice.h"

// Compile the production implementation in this test translation unit so the
// fixture can provide a WARP queue and safely drain it after an injected fault.
// No test hooks or extra public methods are needed in the backend.
#define private public
#include "Backends/D3D12/D3D12Device.h"
#undef private
#include "../src/Backends/D3D12/D3D12Device.cpp"

#include <atomic>
#include <stdexcept>

namespace
{
using dyf::Backends::D3D12Device;
using dyf::Backends::D3D12Texture;
namespace RHI = dyf::RHI;

void Check(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

class LifetimeMarker final : public IUnknown
{
public:
    explicit LifetimeMarker(std::atomic<bool>& destroyed) : m_destroyed(destroyed) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
        *object = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_references; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = --m_references;
        if (remaining == 0) delete this;
        return remaining;
    }

private:
    ~LifetimeMarker() { m_destroyed = true; }
    std::atomic<ULONG> m_references{1};
    std::atomic<bool>& m_destroyed;
};

void ObserveLifetime(ID3D12Resource* source, std::atomic<bool>& destroyed)
{
    static const GUID markerKey =
        {0x657da33b, 0x4d86, 0x4d4e, {0xa3, 0x9f, 0x80, 0x4d, 0xbc, 0xa6, 0xf0, 0xad}};
    ComPtr<IUnknown> marker;
    marker.Attach(new LifetimeMarker(destroyed));
    Check(SUCCEEDED(source->SetPrivateDataInterface(markerKey, marker.Get())),
        "Source lifetime marker attachment failed");
}

// The Windows COM ABI fixes these base-interface vtable layouts. Patching an
// individual object leaves other native devices and fences unaffected.
class VtablePatch
{
public:
    VtablePatch(void* object, size_t count, size_t slot, void* replacement)
        : m_location(static_cast<void***>(object)), m_original(*m_location),
          m_table(m_original, m_original + count)
    {
        m_table[slot] = replacement;
        *m_location = m_table.data();
    }
    ~VtablePatch() { *m_location = m_original; }
    void* Original(size_t slot) const { return m_original[slot]; }

private:
    void*** m_location;
    void** m_original;
    std::vector<void*> m_table;
};

enum class FenceFailure { BeforeSubmission, CompletionRegistration };

class InjectFenceFailure
{
public:
    InjectFenceFailure(ID3D12Device* device, FenceFailure failure) : m_failure(failure)
    {
        Check(s_current == nullptr, "Nested native fence failure injection");
        s_current = this;
        m_devicePatch = std::make_unique<VtablePatch>(device, 44, 36,
            reinterpret_cast<void*>(&CreateFence));
        m_createFence = reinterpret_cast<CreateFenceFunction>(m_devicePatch->Original(36));
    }
    ~InjectFenceFailure()
    {
        m_devicePatch.reset();
        m_fencePatch.reset();
        m_fence.Reset();
        s_current = nullptr;
    }
    ID3D12Fence* Fence() const { return m_fence.Get(); }

private:
    using CreateFenceFunction = HRESULT (STDMETHODCALLTYPE *)(
        ID3D12Device*, UINT64, D3D12_FENCE_FLAGS, REFIID, void**);
    static HRESULT STDMETHODCALLTYPE CreateFence(ID3D12Device* device,
        UINT64 initialValue, D3D12_FENCE_FLAGS flags, REFIID iid, void** output)
    {
        auto& injection = *s_current;
        if (injection.m_failure == FenceFailure::BeforeSubmission)
        {
            *output = nullptr;
            return E_OUTOFMEMORY;
        }
        const HRESULT status = injection.m_createFence(device, initialValue, flags, iid, output);
        if (SUCCEEDED(status) && iid == __uuidof(ID3D12Fence))
        {
            injection.m_fence = static_cast<ID3D12Fence*>(*output);
            injection.m_fencePatch = std::make_unique<VtablePatch>(
                injection.m_fence.Get(), 11, 9, reinterpret_cast<void*>(&SetEventOnCompletion));
        }
        return status;
    }
    static HRESULT STDMETHODCALLTYPE SetEventOnCompletion(ID3D12Fence*, UINT64, HANDLE)
    {
        return E_FAIL;
    }
    static InjectFenceFailure* s_current;
    FenceFailure m_failure;
    CreateFenceFunction m_createFence = nullptr;
    ComPtr<ID3D12Fence> m_fence;
    std::unique_ptr<VtablePatch> m_devicePatch;
    std::unique_ptr<VtablePatch> m_fencePatch;
};
InjectFenceFailure* InjectFenceFailure::s_current = nullptr;

class Fixture
{
public:
    Fixture() : device(std::make_unique<D3D12Device>())
    {
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> warp;
        Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))), "WARP adapter creation failed");
        auto& native = *device->m_internal;
        Check(SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(&native.device))), "WARP D3D12 device creation failed");
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(SUCCEEDED(native.device->CreateCommandQueue(&queueDesc,
            IID_PPV_ARGS(&native.commandQueue))), "Native queue creation failed");
        Check(SUCCEEDED(native.device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(&native.fence))), "Native drain fence creation failed");
        native.fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        Check(native.fenceEvent != nullptr, "Native drain event creation failed");
    }
    ~Fixture()
    {
        if (!device) return;
        if (!Drain())
        {
            // Preserve the test safety reference if real completion is unknown.
            safetySource.Detach();
            device.release();
            return;
        }
        // Test-only recovery after independent native completion verification.
        device->m_internal->submissionFaulted = false;
        device.reset();
    }
    ID3D12Device* NativeDevice() const { return device->m_internal->device.Get(); }
    ID3D12CommandQueue* Queue() const { return device->m_internal->commandQueue.Get(); }
    bool Drain() const
    {
        auto& native = *device->m_internal;
        const uint64_t completion = native.nextCompletionValue++;
        return SUCCEEDED(native.commandQueue->Signal(native.fence.Get(), completion)) &&
            SUCCEEDED(native.fence->SetEventOnCompletion(completion, native.fenceEvent)) &&
            WaitForSingleObject(native.fenceEvent, 10000) == WAIT_OBJECT_0 &&
            native.fence->GetCompletedValue() >= completion &&
            native.fence->GetCompletedValue() != UINT64_MAX;
    }
    D3D12Texture* Texture()
    {
        RHI::TextureDesc desc;
        desc.width = desc.height = 2;
        desc.format = RHI::Format::R8G8B8A8_UNORM;
        desc.usage = RHI::TextureUsage::ShaderResource;
        auto* texture = static_cast<D3D12Texture*>(device->CreateTexture(desc));
        Check(texture != nullptr, "Public texture creation failed");
        // Newly committed D3D12 textures are natively in COMMON.
        texture->SetState(0, 0, RHI::ResourceState::Common);
        return texture;
    }
    std::unique_ptr<D3D12Device> device;
    ComPtr<ID3D12Resource> safetySource;
};

class QueueGate
{
public:
    explicit QueueGate(const Fixture& fixture)
    {
        Check(SUCCEEDED(fixture.NativeDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(&m_gate))), "Queue gate fence creation failed");
        Check(SUCCEEDED(fixture.Queue()->Wait(m_gate.Get(), 1)), "Queue gate submission failed");
    }
    ~QueueGate() { m_gate->Signal(1); }
private:
    ComPtr<ID3D12Fence> m_gate;
};

void CompletionFailureRetainsSourceAfterTextureOwnerRelease()
{
    std::atomic<bool> destroyed{false};
    Fixture fixture;
    auto* texture = fixture.Texture();
    fixture.safetySource = static_cast<ID3D12Resource*>(texture->GetNativeResource());
    ObserveLifetime(fixture.safetySource.Get(), destroyed);
    bool readbackSucceeded = true;
    bool faulted = false;
    bool gpuStillPending = false;
    {
        QueueGate pending(fixture);
        InjectFenceFailure injection(fixture.NativeDevice(), FenceFailure::CompletionRegistration);
        RHI::TextureReadback output;
        readbackSucceeded = fixture.device->ReadTexture(texture, output);
        faulted = fixture.device->IsLostNative();
        gpuStillPending = injection.Fence() && injection.Fence()->GetCompletedValue() == 0;
        fixture.device->DestroyTexture(texture);
    }
    // Keep the real resource safe even when running the unfixed backend. Release
    // this test-owned reference only after the actual submitted copy is drained.
    Check(fixture.Drain(), "Real submitted copy did not complete");
    fixture.safetySource.Reset();
    const bool retainedByBackend = !destroyed;
    Check(!readbackSucceeded && faulted && gpuStillPending,
        "Completion-registration failure did not exercise a pending native copy");
    Check(fixture.device->GetResourceAllocationCounters().textures.live == 0,
        "Public texture owner was not released");
    Check(retainedByBackend,
        "Submitted readback source was released after its public texture owner");
    fixture.device->m_internal->submissionFaulted = false;
    fixture.device.reset();
    Check(destroyed, "Drained device teardown did not release the retained source");
}

void SuccessfulReadbackReleasesSourceWithItsTextureOwner()
{
    std::atomic<bool> destroyed{false};
    Fixture fixture;
    auto* texture = fixture.Texture();
    ObserveLifetime(static_cast<ID3D12Resource*>(texture->GetNativeResource()), destroyed);
    RHI::TextureReadback output;
    Check(fixture.device->ReadTexture(texture, output), "Normal native readback failed");
    Check(output.width == 2 && output.height == 2 && output.rowPitch == 8 &&
        output.format == RHI::Format::R8G8B8A8_UNORM && output.pixels.size() == 16,
        "Normal readback output layout changed");
    fixture.device->DestroyTexture(texture);
    Check(destroyed, "Successful readback retained the source after GPU completion");
    Check(!fixture.device->IsLostNative(), "Successful readback faulted the device");
}

void FailureBeforeSubmissionReleasesSourceWithItsTextureOwner()
{
    std::atomic<bool> destroyed{false};
    Fixture fixture;
    auto* texture = fixture.Texture();
    ObserveLifetime(static_cast<ID3D12Resource*>(texture->GetNativeResource()), destroyed);
    RHI::TextureReadback output;
    {
        InjectFenceFailure injection(fixture.NativeDevice(), FenceFailure::BeforeSubmission);
        Check(!fixture.device->ReadTexture(texture, output), "Pre-submit allocation failure was accepted");
    }
    fixture.device->DestroyTexture(texture);
    Check(destroyed, "Pre-submit readback failure retained its source");
    Check(!fixture.device->IsLostNative(), "Pre-submit readback failure faulted the device");
}
}

int main()
{
    int failures = 0;
    const auto run = [&failures](const char* name, void (*test)())
    {
        try
        {
            test();
            std::printf("PASS: %s\n", name);
        }
        catch (const std::exception& error)
        {
            ++failures;
            std::fprintf(stderr, "FAIL: %s: %s\n", name, error.what());
        }
    };
    run("completion failure retains submitted source", CompletionFailureRetainsSourceAfterTextureOwnerRelease);
    run("successful readback releases source", SuccessfulReadbackReleasesSourceWithItsTextureOwner);
    run("pre-submit failure releases source", FailureBeforeSubmissionReleasesSourceWithItsTextureOwner);
    return failures == 0 ? 0 : 1;
}
