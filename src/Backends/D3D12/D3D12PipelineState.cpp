#include "D3D12PipelineState.h"
#include "D3D12Buffer.h"
#include "D3D12Texture.h"
#include <d3d12.h>
#include <wrl.h>

using Microsoft::WRL::ComPtr;

namespace dy::Backends
{
    // ComPtr들을 보관하는 실제 창고
    struct D3D12PipelineStateInternal
    {
        ComPtr<ID3D12PipelineState> pso;
        ComPtr<ID3D12RootSignature> rootSignature;
    };

    D3D12PipelineState::D3D12PipelineState(
        ID3D12PipelineState* pso,
        ID3D12RootSignature* rootSignature,
        RHI::PrimitiveTopology topology,
        const RHI::VertexBindingDesc* vertexBindings,
        uint32_t vertexBindingCount,
        uint32_t stencilReference,
        std::vector<D3D12ResourceBinding> resourceBindings,
        std::vector<D3D12InlineConstantRange> inlineConstantRanges)
        : m_topology(topology)
        , m_resourceBindings(std::move(resourceBindings))
        , m_inlineConstantRanges(std::move(inlineConstantRanges))
        , m_stencilReference(stencilReference)
    {
        m_internal = new D3D12PipelineStateInternal();
        m_internal->pso = pso;
        m_internal->rootSignature = rootSignature;
        if(vertexBindings != nullptr && vertexBindingCount > 0)
        {
            m_vertexBindings.assign(vertexBindings, vertexBindings + vertexBindingCount);
        }
    }

    D3D12PipelineState::~D3D12PipelineState()
    {
        delete m_internal;
    }

    ID3D12PipelineState* D3D12PipelineState::GetNativePSO() const
    {
        return m_internal->pso.Get();
    }

    ID3D12RootSignature* D3D12PipelineState::GetNativeRootSignature() const
    {
        return m_internal->rootSignature.Get();
    }

    RHI::PrimitiveTopology D3D12PipelineState::GetPrimitiveTopology() const
    {
        return m_topology;
    }

    bool D3D12PipelineState::GetVertexStride(uint32_t slot, uint32_t& stride) const
    {
        for(const RHI::VertexBindingDesc& binding : m_vertexBindings)
        {
            if(binding.slot != slot) continue;
            stride = binding.stride;
            return true;
        }
        return false;
    }

    uint32_t D3D12PipelineState::GetStencilReference() const
    {
        return m_stencilReference;
    }

    struct D3D12Sampler::Internal
    {
        D3D12_SAMPLER_DESC desc = {};
    };

    D3D12Sampler::D3D12Sampler(const RHI::SamplerDesc& desc)
        : RHI::ISampler(desc)
        , m_internal(new Internal())
    {
        if(desc.minFilter == RHI::SamplerFilter::Linear &&
            desc.magFilter == RHI::SamplerFilter::Linear &&
            desc.mipFilter == RHI::SamplerFilter::Linear)
        {
            m_internal->desc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        }
        else if(desc.minFilter == RHI::SamplerFilter::Nearest &&
            desc.magFilter == RHI::SamplerFilter::Nearest &&
            desc.mipFilter == RHI::SamplerFilter::Nearest)
        {
            m_internal->desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        }
        else
        {
            const uint32_t filter =
                (desc.minFilter == RHI::SamplerFilter::Linear ? 0x10u : 0u) |
                (desc.magFilter == RHI::SamplerFilter::Linear ? 0x4u : 0u) |
                (desc.mipFilter == RHI::SamplerFilter::Linear ? 0x1u : 0u);
            m_internal->desc.Filter = static_cast<D3D12_FILTER>(filter);
        }
        const RHI::SamplerAddressMode addressModes[] = { desc.addressU, desc.addressV, desc.addressW };
        D3D12_TEXTURE_ADDRESS_MODE* nativeModes[] = {
            &m_internal->desc.AddressU,
            &m_internal->desc.AddressV,
            &m_internal->desc.AddressW
        };
        for(uint32_t index = 0; index < 3; ++index)
        {
            switch(addressModes[index])
            {
            case RHI::SamplerAddressMode::Repeat: *nativeModes[index] = D3D12_TEXTURE_ADDRESS_MODE_WRAP; break;
            case RHI::SamplerAddressMode::MirroredRepeat: *nativeModes[index] = D3D12_TEXTURE_ADDRESS_MODE_MIRROR; break;
            case RHI::SamplerAddressMode::ClampToEdge: *nativeModes[index] = D3D12_TEXTURE_ADDRESS_MODE_CLAMP; break;
            }
        }
        m_internal->desc.MaxAnisotropy = 1;
        m_internal->desc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        m_internal->desc.MinLOD = desc.minLod;
        m_internal->desc.MaxLOD = desc.maxLod;
    }

    const void* D3D12Sampler::GetNativeDesc() const
    {
        return &m_internal->desc;
    }

    D3D12Sampler::~D3D12Sampler()
    {
        delete m_internal;
    }

    struct D3D12ResourceSet::Internal
    {
        D3D12PipelineState* pipeline = nullptr;
        ComPtr<ID3D12DescriptorHeap> resourceHeap;
        ComPtr<ID3D12DescriptorHeap> samplerHeap;
        uint32_t resourceDescriptorSize = 0;
        uint32_t samplerDescriptorSize = 0;
        bool valid = false;
    };

    D3D12ResourceSet::D3D12ResourceSet(ID3D12Device* device, D3D12PipelineState* pipeline)
        : m_internal(new Internal())
    {
        m_internal->pipeline = pipeline;
        if(device == nullptr || pipeline == nullptr) return;
        uint32_t resourceCount = 0;
        uint32_t samplerCount = 0;
        for(const D3D12ResourceBinding& binding : pipeline->GetResourceBindings())
        {
            resourceCount += binding.desc.arrayCount;
            if(binding.desc.type == RHI::ResourceType::TextureSampler) samplerCount += binding.desc.arrayCount;
        }
        if(resourceCount > 0)
        {
            D3D12_DESCRIPTOR_HEAP_DESC desc = {};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            desc.NumDescriptors = resourceCount;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if(FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_internal->resourceHeap)))) return;
            m_internal->resourceDescriptorSize = device->GetDescriptorHandleIncrementSize(desc.Type);
        }
        if(samplerCount > 0)
        {
            D3D12_DESCRIPTOR_HEAP_DESC desc = {};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
            desc.NumDescriptors = samplerCount;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if(FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_internal->samplerHeap)))) return;
            m_internal->samplerDescriptorSize = device->GetDescriptorHandleIncrementSize(desc.Type);
        }
        m_internal->valid = true;
    }

    D3D12ResourceSet::~D3D12ResourceSet()
    {
        delete m_internal;
    }

    bool D3D12ResourceSet::IsValid() const
    {
        return m_internal->valid;
    }

    bool D3D12ResourceSet::Update(ID3D12Device* device, const RHI::ResourceSetWrite* writes, uint32_t writeCount)
    {
        if(!m_internal->valid || device == nullptr || (writeCount > 0 && writes == nullptr)) return false;
        for(uint32_t writeIndex = 0; writeIndex < writeCount; ++writeIndex)
        {
            const RHI::ResourceSetWrite& write = writes[writeIndex];
            const D3D12ResourceBinding* declared = nullptr;
            for(const D3D12ResourceBinding& binding : m_internal->pipeline->GetResourceBindings())
            {
                if(binding.desc.set == write.set && binding.desc.binding == write.binding)
                {
                    declared = &binding;
                    break;
                }
            }
            if(declared == nullptr || write.arrayElement >= declared->desc.arrayCount) return false;

            D3D12_CPU_DESCRIPTOR_HANDLE resourceHandle = m_internal->resourceHeap->GetCPUDescriptorHandleForHeapStart();
            resourceHandle.ptr += static_cast<SIZE_T>(declared->heapOffset + write.arrayElement) * m_internal->resourceDescriptorSize;
            if(declared->desc.type == RHI::ResourceType::TextureSampler)
            {
                auto* texture = dynamic_cast<D3D12Texture*>(write.texture);
                auto* sampler = dynamic_cast<D3D12Sampler*>(write.sampler);
                if(texture == nullptr || sampler == nullptr || write.buffer != nullptr) return false;
                D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
                srv.Format = static_cast<DXGI_FORMAT>(D3D12Texture::ToDxgiShaderResourceFormat(texture->GetFormat()));
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Texture2D.MipLevels = texture->GetMipLevels();
                device->CreateShaderResourceView(static_cast<ID3D12Resource*>(texture->GetNativeResource()), &srv, resourceHandle);

                D3D12_CPU_DESCRIPTOR_HANDLE samplerHandle = m_internal->samplerHeap->GetCPUDescriptorHandleForHeapStart();
                samplerHandle.ptr += static_cast<SIZE_T>(declared->samplerHeapOffset + write.arrayElement) * m_internal->samplerDescriptorSize;
                device->CreateSampler(static_cast<const D3D12_SAMPLER_DESC*>(sampler->GetNativeDesc()), samplerHandle);
                continue;
            }

            auto* buffer = dynamic_cast<D3D12Buffer*>(write.buffer);
            if(buffer == nullptr || write.texture != nullptr || write.sampler != nullptr || write.bufferOffset >= buffer->GetSize()) return false;
            const uint32_t available = buffer->GetSize() - write.bufferOffset;
            const uint32_t size = write.bufferSize == 0 ? available : write.bufferSize;
            if(size == 0 || size > available) return false;
            ID3D12Resource* resource = static_cast<ID3D12Resource*>(buffer->GetNativeResource());
            if(declared->desc.type == RHI::ResourceType::ConstantBuffer)
            {
                if((write.bufferOffset & 255u) != 0) return false;
                D3D12_CONSTANT_BUFFER_VIEW_DESC cbv = {};
                cbv.BufferLocation = resource->GetGPUVirtualAddress() + write.bufferOffset;
                cbv.SizeInBytes = (size + 255u) & ~255u;
                device->CreateConstantBufferView(&cbv, resourceHandle);
            }
            else
            {
                if(buffer->GetStride() == 0 || write.bufferOffset % buffer->GetStride() != 0 || size % buffer->GetStride() != 0) return false;
                D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
                srv.Format = DXGI_FORMAT_UNKNOWN;
                srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Buffer.FirstElement = write.bufferOffset / buffer->GetStride();
                srv.Buffer.NumElements = size / buffer->GetStride();
                srv.Buffer.StructureByteStride = buffer->GetStride();
                device->CreateShaderResourceView(resource, &srv, resourceHandle);
            }
        }
        return true;
    }

    void D3D12ResourceSet::Bind(ID3D12GraphicsCommandList* commandList, D3D12PipelineState* activePipeline)
    {
        if(!m_internal->valid || commandList == nullptr || activePipeline != m_internal->pipeline) return;
        ID3D12DescriptorHeap* heaps[2] = {};
        uint32_t heapCount = 0;
        if(m_internal->resourceHeap != nullptr) heaps[heapCount++] = m_internal->resourceHeap.Get();
        if(m_internal->samplerHeap != nullptr) heaps[heapCount++] = m_internal->samplerHeap.Get();
        if(heapCount > 0) commandList->SetDescriptorHeaps(heapCount, heaps);

        for(const D3D12ResourceBinding& binding : m_internal->pipeline->GetResourceBindings())
        {
            D3D12_GPU_DESCRIPTOR_HANDLE resourceHandle = m_internal->resourceHeap->GetGPUDescriptorHandleForHeapStart();
            commandList->SetGraphicsRootDescriptorTable(binding.resourceRootParameter, resourceHandle);
            if(binding.desc.type != RHI::ResourceType::TextureSampler) continue;
            D3D12_GPU_DESCRIPTOR_HANDLE samplerHandle = m_internal->samplerHeap->GetGPUDescriptorHandleForHeapStart();
            commandList->SetGraphicsRootDescriptorTable(binding.samplerRootParameter, samplerHandle);
        }
    }
}
