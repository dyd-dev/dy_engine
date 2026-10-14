#include "dyf/RHI/IDevice.h"
#define private public
#include "Backends/D3D12/D3D12Device.h"
#include "Backends/D3D12/D3D12ResourceSet.h"
#include "Backends/D3D12/D3D12Texture.h"
#undef private
#include "../src/Backends/D3D12/D3D12Device.cpp"
#include "../src/Backends/D3D12/D3D12ResourceSet.cpp"
#define HasUsage TextureHasUsage
#include "../src/Backends/D3D12/D3D12Texture.cpp"
#undef HasUsage
#include "P2BackendTestAllocation.h"

namespace RHI = dyf::RHI;
using namespace dyf::Backends;
namespace
{
unsigned failures = 0;
void Check(bool value, const char* message)
{
    if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}

void VisibilityLimits(D3D12Device& device)
{
    RHI::ResourceBindingLayout bindings[2]{};
    bindings[0].type = bindings[1].type = RHI::ResourceBindingType::ConstantBuffer;
    bindings[0].stages = RHI::ShaderStageFlags::Vertex | RHI::ShaderStageFlags::Fragment;
    bindings[1].stages = RHI::ShaderStageFlags::Hull | RHI::ShaderStageFlags::Domain;
    bindings[0].count = 7;
    bindings[1].count = 7;
    bindings[1].binding = 7;
    RHI::PipelineLayoutDesc layout{};
    layout.bindings = bindings;
    layout.bindingCount = 2;
    for (auto tier : {D3D12_RESOURCE_BINDING_TIER_1, D3D12_RESOURCE_BINDING_TIER_2})
    {
        device.m_internal->resourceBindingTier = tier;
        Check(device.SupportsPipelineLayoutNative(layout), "14 effective CBVs should fit");
        bindings[1].count = 8;
        Check(!device.SupportsPipelineLayoutNative(layout), "15 effective CBVs must exceed Tier 1/2 limit");
        bindings[1].count = 7;
    }
    device.m_internal->resourceBindingTier = D3D12_RESOURCE_BINDING_TIER_3;
    bindings[1].count = 8;
    Check(device.SupportsPipelineLayoutNative(layout), "Tier 3 must retain its larger CBV limit");
    device.m_internal->resourceBindingTier = D3D12_RESOURCE_BINDING_TIER_1;
    bindings[0].type = bindings[1].type = RHI::ResourceBindingType::SampledTexture;
    bindings[0].count = 64;
    bindings[1].count = 64;
    bindings[1].binding = 64;
    Check(device.SupportsPipelineLayoutNative(layout), "128 effective SRVs should fit");
    bindings[1].count = 65;
    Check(!device.SupportsPipelineLayoutNative(layout), "129 effective SRVs must exceed Tier 1 limit");
    bindings[0].stages = RHI::ShaderStageFlags::Vertex;
    bindings[1].stages = RHI::ShaderStageFlags::Fragment;
    Check(device.SupportsPipelineLayoutNative(layout), "Separate single-stage SRVs must remain valid");
}

void ResourceSetOwnership(ID3D12Device* native)
{
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> heap;
    if (FAILED(native->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)))) throw std::runtime_error("Heap creation failed");
    const auto references = [&]() { heap->AddRef(); return heap->Release(); };
    const ULONG baseline = references();
    const std::vector<ID3D12Resource*> resources{nullptr, nullptr};
    bool threw = false;
    P2Allocation::Begin(1); // internal allocation succeeds; native-resource array fails
    try { D3D12ResourceSet set({}, heap.Get(), 32, resources); }
    catch (const std::bad_alloc&) { threw = true; }
    const unsigned leaked = P2Allocation::End();
    Check(threw, "ResourceSet failure injection must be reached");
    Check(leaked == 0, "ResourceSet constructor must release its internal allocation");
    Check(references() == baseline, "ResourceSet constructor must release its heap reference on failure");
    for (const auto& input : {std::vector<ID3D12Resource*>{}, resources})
    {
        { D3D12ResourceSet set({}, heap.Get(), 32, input);
          Check(set.GetNativeDescriptorHeap() == heap.Get() && set.GetDescriptorSize() == 32 &&
                set.GetNativeResourceCount() == input.size(), "ResourceSet success must preserve descriptors"); }
        Check(references() == baseline, "ResourceSet success must balance the heap reference");
    }
}

void TextureOwnership()
{
    RHI::TextureDesc desc{};
    desc.width = desc.height = 1;
    desc.mipLevels = desc.depthOrArraySize = 1;
    for (bool wrap : {false, true})
    {
        bool threw = false;
        P2Allocation::Begin(1); // exercise a failure after one constructor allocation
        try
        {
            if (wrap) { D3D12Texture texture(static_cast<ID3D12Resource*>(nullptr), desc, 99, true); }
            else { D3D12Texture texture(static_cast<ID3D12Device*>(nullptr), desc); }
        }
        catch (const std::bad_alloc&) { threw = true; }
        const unsigned leaked = P2Allocation::End();
        Check(threw, "Texture allocation failure must be reached");
        Check(leaked == 0, "Texture constructor must not leak its internal block");
    }
    D3D12Texture texture(static_cast<ID3D12Resource*>(nullptr), desc, 99, true);
    Check(texture.IsSwapchainImage() && texture.GetState(0, 0) == RHI::ResourceState::Present &&
        texture.GetRenderTargetViewHandle(0, 0) == 99, "Wrapping success must preserve state and RTV");
}
}

int main() try
{
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) throw std::runtime_error("WARP unavailable");
    D3D12Device device;
    if (FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device.m_internal->device))))
        throw std::runtime_error("WARP D3D12 device unavailable");
    VisibilityLimits(device);
    ResourceSetOwnership(device.m_internal->device.Get());
    TextureOwnership();
    std::printf("P2 D3D12: %u failures\n", failures);
    return failures ? 1 : 0;
}
catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 2; }
