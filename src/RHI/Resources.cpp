#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/ICommandList.h"
#include "RHI/Validation.h"
#include "dyf/RHI/Readback.h"
#include "dyf/Core/ThreadPool.h"
#include <chrono>
#include <thread>
#include <cstdio>

namespace dyf::RHI
{
IDevice::~IDevice() = default;

void IDevice::ReportDiagnostic(DiagnosticSeverity severity, const char* message) const
{
    std::fprintf(stderr, "dyf::RHI [%s]: %s\n",
        severity == DiagnosticSeverity::Error ? "error" :
        severity == DiagnosticSeverity::Warning ? "warning" : "info", message);
}

uint64_t IDevice::GetLimit(Limit limit) const { return GetLimitNative(limit); }

bool IDevice::Supports(const SamplerDesc& desc) const
{
    return IsValidSampler(desc) && desc.maxAnisotropy <= GetLimit(Limit::SamplerAnisotropy) &&
        (desc.mipLodBias == 0 || Supports(Feature::SamplerLodBias)) && SupportsSamplerNative(desc);
}

bool IDevice::Supports(const PipelineLayoutDesc& desc) const
{
    if(!ValidatePipelineLayout(desc) || desc.inlineConstantSize > GetLimit(Limit::InlineConstantBytes)) return false;
    for(uint32_t i = 0; i < desc.bindingCount; ++i)
    {
        const auto& binding = desc.bindings[i];
        if(binding.type == ResourceBindingType::StaticSampler && !Supports(binding.staticSampler)) return false;
        if(binding.type == ResourceBindingType::SampledTexture && binding.count > 1 &&
            !Supports(Feature::DescriptorIndexing)) return false;
    }
    return SupportsPipelineLayoutNative(desc);
}

bool IDevice::Supports(const GraphicsPipelineDesc& desc) const
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    if(!Reference(desc.vertexShader) || (desc.fragmentShader && !Reference(desc.fragmentShader))) return false;
    if(!Supports(desc.layout) || !ValidateGraphicsPipelineDesc(desc)) return false;
    if(desc.raster.fillMode == FillMode::Wireframe && !Supports(Feature::Wireframe)) return false;
    if(desc.raster.depthBiasClamp != 0 && !Supports(Feature::DepthBiasClamp)) return false;
    if(std::trunc(desc.raster.depthBiasConstant) != desc.raster.depthBiasConstant &&
        !Supports(Feature::FractionalDepthBias)) return false;
    return SupportsGraphicsPipelineNative(desc);
}

TimestampQueryHandle IDevice::CreateTimestampQuery(const TimestampQueryDesc& desc)
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    if(!desc.count || !Supports(Feature::TimestampQuery)) return nullptr;
    auto* query=CreateTimestampQueryNative(desc);
    Track(query,[](IDevice& device,void* object){device.DestroyTimestampQueryNative(static_cast<TimestampQueryHandle>(object));});
    return query;
}
void IDevice::DestroyTimestampQuery(TimestampQueryHandle query)
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    m_resources.erase(query);
}
bool IDevice::ReadTimestamps(TimestampQueryHandle query,uint32_t first,uint32_t count,uint64_t* ticks)
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    if(!Reference(query) || !ticks || !count || first>=query->GetCount() || count>query->GetCount()-first) return false;
    return ReadTimestampsNative(query,first,count,ticks);
}

void IDevice::InvalidateBackBuffers()
{
    ++m_imageGeneration;
    for(const auto* texture : m_borrowedTextures)
    {
        m_resources.erase(texture);
        m_resourceStates.erase({reinterpret_cast<uintptr_t>(texture),0,0});
    }
    m_borrowedTextures.clear();
}

bool IDevice::IsLost() const
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    return IsLostNative();
}

bool IDevice::WaitIdle()
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    return WaitIdleNative();
}

bool IDevice::CreateSwapchain(const SwapchainDesc& desc)
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    if(m_frameActive || desc.format == Format::Unknown || !desc.minimumImageCount ||
        (desc.presentMode != PresentMode::Fifo && desc.presentMode != PresentMode::Immediate && desc.presentMode != PresentMode::Mailbox)) return false;
    const bool replacing = m_hasSwapchain;
    const auto previous = m_swapchainDesc;
    if(replacing)
    {
        if(!WaitIdleNative()) return false;
        for(const auto* texture : m_borrowedTextures)
        {
            const auto reference = m_resources.find(texture);
            if(reference != m_resources.end() && reference->second.use_count() != 1) return false;
        }
        InvalidateBackBuffers();
        DestroySwapchainNative();
        m_hasSwapchain = false;
    }
    if(!CreateSwapchainNative(desc))
    {
        ReportDiagnostic(DiagnosticSeverity::Error, "CreateSwapchain: native creation rejected the requested settings or allocation failed. No fallback settings were applied.");
        if(replacing) m_hasSwapchain = CreateSwapchainNative(previous);
        return false;
    }
    m_swapchainDesc = desc;
    m_hasSwapchain = true;
    return true;
}

bool IDevice::BeginFrame()
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    if(m_frameActive) return true;
    InvalidateBackBuffers();
    m_frameActive = BeginFrameNative();
    return m_frameActive;
}

bool IDevice::Present()
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    if(!m_frameActive) {std::fprintf(stderr,"dyf::RHI: Present requires an active frame.\n");return false;}
    auto* backBuffer = GetBackBuffer();
    const auto state = m_resourceStates.find({reinterpret_cast<uintptr_t>(backBuffer),0,0});
    if(!backBuffer || state == m_resourceStates.end() || state->second != ResourceState::Present)
    {std::fprintf(stderr,"dyf::RHI: Submit the backbuffer transition to Present before presenting.\n");return false;}
    const bool presented = PresentNative();
    m_frameActive = false;
    if(!presented || IsLostNative()) {std::fprintf(stderr,"dyf::RHI: Presentation failed.\n");return false;}
    return true;
}


std::shared_ptr<void> IDevice::Reference(const void* handle) const
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    const auto found = m_resources.find(handle);
    return found == m_resources.end() ? nullptr : found->second;
}

void IDevice::Track(void* handle, void (*destroy)(IDevice&, void*),
    std::vector<std::shared_ptr<void>> dependencies)
{
    if(!handle) return;
    auto reference = std::shared_ptr<void>(handle,
        [this, destroy, dependencies = std::move(dependencies)](void* object)
        {
            std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
            const uintptr_t key=reinterpret_cast<uintptr_t>(object);
            for(auto it=m_resourceStates.begin();it!=m_resourceStates.end();)
                if(std::get<0>(it->first)==key)it=m_resourceStates.erase(it);else ++it;
            if(m_nativeResourcesAlive) destroy(*this, object);
        });
    m_resources.emplace(handle, std::move(reference));
}

ResourceAllocationCounters IDevice::GetResourceAllocationCounters() const
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    return m_allocationCounters;
}

void IDevice::AbandonResources()
{
    m_nativeResourcesAlive = false;
    ReleaseResources();
}

void IDevice::ReleaseResources()
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    std::vector<std::shared_ptr<void>> references;
    for(auto* commands : m_recordedCommands)
    {
        for(auto& reference : commands->m_references) references.push_back(std::move(reference));
        commands->m_references.clear();
        commands->m_owner = nullptr;
    }
    m_recordedCommands.clear();
    for(auto* commands : m_userCommands) delete commands;
    m_userCommands.clear();
    m_resources.clear();
    m_resourceStates.clear();
    m_borrowedTextures.clear();
}

ICommandList* IDevice::AcquireCommandList()
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    auto* commands = ICommandList::CreateRecorded();
    m_userCommands.insert(commands);
    if(commands)
    {
        commands->m_owner = this;
        m_recordedCommands.insert(commands);
    }
    return commands;
}

TextureHandle IDevice::GetBackBuffer()
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    auto* texture = GetBackBufferNative();
    if(texture && !Reference(texture))
    {
        m_borrowedTextures.insert(texture);
        m_resources.emplace(texture, std::shared_ptr<void>(texture, [](void*) {}));
        m_resourceStates[{reinterpret_cast<uintptr_t>(texture),0,0}]=ResourceState::Present;
    }
    return texture;
}



bool IDevice::Submit(const SubmitDesc& desc, FenceHandle& completion)
{
    completion={};
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    if(!desc.commandLists || !desc.commandListCount || (desc.waitCount && !desc.waits)) return false;
    // One queue is exposed by the current raster implementation. Earlier
    // completion points on this same queue are ordered without a CPU stall.
    for(uint32_t i=0;i<desc.waitCount;++i)
        if(desc.waits[i].m_device!=this || !desc.waits[i].m_value || desc.waits[i].m_value>GetLastSubmissionNative()) return false;
    std::vector<ICommandList*> commands(desc.commandLists,desc.commandLists+desc.commandListCount);
    if(!Submit(commands.data(),desc.commandListCount)) return false;
    completion.m_device=this;
    completion.m_value=GetLastSubmissionNative();
    return true;
}
bool IDevice::IsComplete(FenceHandle completion)
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    return completion.m_device==this && completion.m_value && completion.m_value<=GetCompletedSubmissionNative();
}
bool IDevice::Wait(FenceHandle completion,uint64_t timeoutNanoseconds)
{
    if(completion.m_device!=this || !completion.m_value) return false;
    const auto start=std::chrono::steady_clock::now();
    do
    {
        if(IsComplete(completion)) return true;
        if(IsLost()) return false;
        if(timeoutNanoseconds==0) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    } while(timeoutNanoseconds==UINT64_MAX || static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now()-start).count())<timeoutNanoseconds);
    return false;
}

bool IDevice::ResetCommandList(ICommandList* commands)
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    if(!commands || !m_userCommands.count(commands) ||
        (commands->m_completion && commands->m_completion>GetCompletedSubmissionNative())) return false;
    if(commands->m_preparedNative)
    {
        DiscardCommandListNative(commands->m_preparedNative);
        commands->m_preparedNative = nullptr;
    }
    commands->m_commands.clear();
    commands->m_stateOperations.clear();
    commands->m_references.clear();
    commands->m_recordingClosed=commands->m_recordingFailed=false;
    commands->m_debugEventDepth=0;
    commands->m_rendering=commands->m_viewport=commands->m_scissor=false;
    commands->m_pipeline=nullptr;
    commands->m_indexBuffer=nullptr;
    commands->m_resourceSet=nullptr;
    commands->m_vertexBuffers.clear();
    commands->m_colorFormats.clear();
    commands->m_depthStencilFormat=Format::Unknown;
    commands->m_completion=commands->m_imageGeneration=0;
    return true;
}
void IDevice::DestroyCommandList(ICommandList* commands)
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    if(commands && m_userCommands.erase(commands)) delete commands;
}

bool IDevice::PrepareCommandLists(ICommandList* const* commands, uint32_t count, Core::ThreadPool* pool)
{
    if(!commands || !count) return false;
    struct Work { ICommandList* source; ICommandList* native; };
    std::vector<Work> work;
    work.reserve(count);
    const auto discard = [&] {
        std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
        for(const auto& item : work) DiscardCommandListNative(item.native);
    };
    try
    {
        {
            std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
            const uint64_t completed = GetCompletedSubmissionNative();
            for(uint32_t i = 0; i < count; ++i)
            {
                if(!commands[i] || !m_userCommands.count(commands[i]) ||
                    !commands[i]->m_recordingClosed || commands[i]->m_recordingFailed ||
                    commands[i]->m_completion > completed) return false;
                for(uint32_t j = 0; j < i; ++j) if(commands[i] == commands[j]) return false;
            }
            // Native pools/allocators are allocated under the device lock. Each list
            // has its own recording storage; workers never mutate the ownership sets.
            for(uint32_t i = 0; i < count; ++i)
            {
                if(commands[i]->m_preparedNative) continue;
                auto* native = AcquireCommandListNative();
                if(!native) { discard(); return false; }
                work.push_back({commands[i], native});
                native->m_owner = this;
                m_recordedCommands.insert(native);
                native->m_references = commands[i]->m_references;
            }
        }
        std::vector<uint8_t> recorded(work.size(), 0);
        const auto record = [&](size_t i) {
            recorded[i] = work[i].native->ReplayNative(work[i].source->m_commands);
        };
        if(pool && pool->GetThreadCount() > 1 && !pool->IsWorkerThread() && work.size() > 1)
        {
            std::vector<std::future<void>> pending;
            pending.reserve(work.size());
            std::exception_ptr failure;
            try
            {
                for(size_t i = 0; i < work.size(); ++i)
                    pending.push_back(pool->Enqueue([&, i] { record(i); }));
            }
            catch(...) { failure = std::current_exception(); }
            for(auto& job : pending)
                try { job.get(); }
                catch(...) { if(!failure) failure = std::current_exception(); }
            if(failure) std::rethrow_exception(failure);
        }
        else for(size_t i = 0; i < work.size(); ++i) record(i);
        for(auto success : recorded)
            if(!success)
            {
                ReportDiagnostic(DiagnosticSeverity::Error, "PrepareCommandLists: native recording failed.");
                discard();
                return false;
            }
        for(const auto& item : work) item.source->m_preparedNative = item.native;
        return true;
    }
    catch(...) { discard(); throw; }
}

bool IDevice::Submit(ICommandList** commands, uint32_t count)
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    if(!commands || !count) return false;
    const uint64_t completed=GetCompletedSubmissionNative();
    for(uint32_t i=0;i<count;++i)
    {
        if(!commands[i] || !m_userCommands.count(commands[i]) || !commands[i]->m_recordingClosed ||
            commands[i]->m_recordingFailed || commands[i]->m_completion>completed ||
            (commands[i]->m_imageGeneration && !m_frameActive) ||
            (commands[i]->m_imageGeneration && commands[i]->m_imageGeneration!=m_imageGeneration)) return false;
        for(uint32_t j=0;j<i;++j) if(commands[i]==commands[j]) return false;
    }
    auto states=m_resourceStates;
    for(uint32_t i=0;i<count;++i)for(const auto& validate:commands[i]->m_stateOperations)
        if(!validate(states))
        {
            ReportDiagnostic(DiagnosticSeverity::Error, "Submit: resource state mismatch. Barrier.before must match the current state (Undefined is not a wildcard); bound resources must still have the required state at draw/dispatch.");
            return false;
        }
    std::vector<ICommandList*> native(count);
    if(!PrepareCommandLists(commands, count)) return false;
    for(uint32_t i=0;i<count;++i)
    {
        native[i]=commands[i]->m_preparedNative;
        // SubmitNative takes ownership and may destroy a failed native list.
        commands[i]->m_preparedNative=nullptr;
    }
    const auto discard=[&] {for(auto* list:native) DiscardCommandListNative(list);};
    if(!SubmitNative(native.data(),count))
    {
        ReportDiagnostic(DiagnosticSeverity::Error,"Submit: native submission failed.");
        discard();
        return false;
    }
    for(const auto& entry:states) {
        const auto found=m_resourceStates.find(entry.first);
        if(found!=m_resourceStates.end())found->second=entry.second;
    }
    const uint64_t completion=GetLastSubmissionNative();
    for(uint32_t i=0;i<count;++i) commands[i]->m_completion=completion;
    return true;
}

BufferHandle IDevice::CreateBuffer(const BufferDesc& desc)
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    if(!desc.size || desc.usage == BufferUsage::None || !IsBufferStateAllowed(desc, desc.initialState)) return nullptr;
    auto* buffer = CreateBufferNative(desc);
    if(!buffer) ReportDiagnostic(DiagnosticSeverity::Error,"CreateBuffer: native size, usage or initial-state constraints (or allocation failure) prevented creation.");
    if(buffer) { ++m_allocationCounters.buffers.live; ++m_allocationCounters.buffers.created; }
    Track(buffer, [](IDevice& device, void* handle) {
        device.DestroyBufferNative(static_cast<BufferHandle>(handle));
        --device.m_allocationCounters.buffers.live; ++device.m_allocationCounters.buffers.destroyed;
    });
    if(buffer)m_resourceStates[{reinterpret_cast<uintptr_t>(buffer),0,0}]=desc.initialState;
    return buffer;
}

TextureHandle IDevice::CreateTexture(const TextureDesc& desc)
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    if(!desc.width || !desc.height || !desc.depthOrArraySize || !desc.mipLevels
        || desc.width > GetLimit(Limit::Texture2DDimension) || desc.height > GetLimit(Limit::Texture2DDimension)
        || desc.mipLevels > MaximumMipCount(desc.width, desc.height)
        || !FormatSize(desc.format) || desc.usage == TextureUsage::None) return nullptr;
    auto* texture = CreateTextureNative(desc);
    if(!texture) ReportDiagnostic(DiagnosticSeverity::Error,"CreateTexture: native format, extent or usage constraints (or allocation failure) prevented creation; no format was substituted.");
    if(texture) { ++m_allocationCounters.textures.live; ++m_allocationCounters.textures.created; }
    Track(texture, [](IDevice& device, void* handle) {
        device.DestroyTextureNative(static_cast<TextureHandle>(handle));
        --device.m_allocationCounters.textures.live; ++device.m_allocationCounters.textures.destroyed;
    });
    if(texture)for(uint32_t layer=0;layer<desc.depthOrArraySize;++layer)for(uint32_t mip=0;mip<desc.mipLevels;++mip)
        m_resourceStates[{reinterpret_cast<uintptr_t>(texture),mip,layer}]=ResourceState::Undefined;
    return texture;
}

ShaderHandle IDevice::CreateShader(const ShaderDesc& desc)
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    if((desc.stage==ShaderStage::Compute && !Supports(Feature::Compute)) || desc.stage == ShaderStage::Unknown || desc.stage>ShaderStage::Compute || !desc.binary || !desc.binarySize || !desc.entryPoint || !*desc.entryPoint) return nullptr;
    auto* shader = CreateShaderNative(desc);
    if(!shader) ReportDiagnostic(DiagnosticSeverity::Error,"CreateShader: native shader creation failed; no substitute shader was selected.");
    Track(shader, [](IDevice& device, void* handle) { device.DestroyShaderNative(static_cast<ShaderHandle>(handle)); });
    return shader;
}


PipelineHandle IDevice::CreateComputePipeline(const ComputePipelineDesc& desc)
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    if(!Supports(Feature::Compute))
    {ReportDiagnostic(DiagnosticSeverity::Error,"CreateComputePipeline: this backend/device does not support Compute.");return nullptr;}
    auto shader=Reference(desc.computeShader);
    if(!shader || desc.computeShader->GetStage()!=ShaderStage::Compute ||
        !Supports(desc.layout) ||
        (desc.layout.inlineConstantSize && desc.layout.inlineConstantStages!=ShaderStageFlags::Compute))return nullptr;
    for(uint32_t i=0;i<desc.layout.bindingCount;++i)
        if(desc.layout.bindings[i].stages!=ShaderStageFlags::Compute)return nullptr;
    auto* pipeline=CreateComputePipelineNative(desc);
    if(!pipeline) ReportDiagnostic(DiagnosticSeverity::Error,"CreateComputePipeline: native creation failed; no substitute pipeline was selected.");
    if(pipeline) { ++m_allocationCounters.pipelines.live; ++m_allocationCounters.pipelines.created; }
    Track(pipeline,[](IDevice& device,void* handle){
        device.DestroyPipelineNative(static_cast<PipelineHandle>(handle));
        --device.m_allocationCounters.pipelines.live; ++device.m_allocationCounters.pipelines.destroyed;
    },{shader});
    return pipeline;
}

PipelineHandle IDevice::CreateGraphicsPipeline(const GraphicsPipelineDesc& desc)
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    auto vertex = Reference(desc.vertexShader), fragment = Reference(desc.fragmentShader);
    if(!vertex || (desc.fragmentShader && !fragment) || !Supports(desc))
    {ReportDiagnostic(DiagnosticSeverity::Error,"CreateGraphicsPipeline: unsupported description or device limit. Query Supports(desc) before creation.");return nullptr;}
    auto* pipeline = CreateGraphicsPipelineNative(desc);
    if(!pipeline) ReportDiagnostic(DiagnosticSeverity::Error,"CreateGraphicsPipeline: native creation failed; no fallback pipeline was substituted.");
    if(pipeline)
    {
        for(uint32_t i=0;i<desc.vertexAttributeCount;++i)
        {
            const uint32_t binding=desc.vertexAttributes[i].binding;
            if(std::find(pipeline->m_requiredVertexBindings.begin(),pipeline->m_requiredVertexBindings.end(),binding)==pipeline->m_requiredVertexBindings.end())
                pipeline->m_requiredVertexBindings.push_back(binding);
        }
        for(uint32_t i=0;i<desc.colorAttachmentCount;++i)
            pipeline->m_colorFormats.push_back(desc.colorAttachments[i].format);
        pipeline->m_depthStencilFormat=desc.depthStencil.format;
    }
    if(pipeline) { ++m_allocationCounters.pipelines.live; ++m_allocationCounters.pipelines.created; }
    Track(pipeline, [](IDevice& device, void* handle) {
        device.DestroyPipelineNative(static_cast<PipelineHandle>(handle));
        --device.m_allocationCounters.pipelines.live; ++device.m_allocationCounters.pipelines.destroyed;
    }, {vertex, fragment});
    return pipeline;
}

ResourceSetHandle IDevice::CreateResourceSet(const ResourceSetDesc& desc)
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    std::vector<std::shared_ptr<void>> dependencies;
    auto pipeline = Reference(desc.pipeline);
    if(!pipeline || (desc.bindingCount && !desc.bindings)) return nullptr;
    dependencies.push_back(std::move(pipeline));
    for(uint32_t i = 0; i < desc.bindingCount; ++i)
    {
        const auto& binding = desc.bindings[i];
        if(binding.buffer)
        {
            auto reference = Reference(binding.buffer);
            if(!reference) return nullptr;
            dependencies.push_back(std::move(reference));
        }
        if(binding.texture)
        {
            auto reference = Reference(binding.texture);
            if(!reference) return nullptr;
            dependencies.push_back(std::move(reference));
        }
    }
    if(!ValidateResourceSetDesc(desc)) return nullptr;
    const auto& layout = desc.pipeline->GetLayout();
    for(uint32_t i = 0; i < desc.bindingCount; ++i)
    {
        const auto& binding = desc.bindings[i];
        if(!binding.buffer) continue;
        const auto found = std::find_if(layout.bindings, layout.bindings + layout.bindingCount,
            [&](const auto& entry) { return entry.binding == binding.binding; });
        if(found == layout.bindings + layout.bindingCount) return nullptr;
        const bool uniform = found->type == ResourceBindingType::ConstantBuffer;
        const auto alignment = GetLimit(uniform ? Limit::UniformBufferOffsetAlignment : Limit::StorageBufferOffsetAlignment);
        const auto maximum = GetLimit(uniform ? Limit::UniformBufferBytes : Limit::StorageBufferBytes);
        if(!alignment || binding.offset % alignment || binding.size > maximum)
        {ReportDiagnostic(DiagnosticSeverity::Error,"CreateResourceSet: buffer binding exceeds device range or offset alignment. Query GetLimit for the required values.");return nullptr;}
    }
    auto* resources = CreateResourceSetNative(desc);
    if(!resources) ReportDiagnostic(DiagnosticSeverity::Error,"CreateResourceSet: native binding constraints or allocation prevented creation; no substitute resource was bound.");
    Track(resources, [](IDevice& device, void* handle) { device.DestroyResourceSetNative(static_cast<ResourceSetHandle>(handle)); }, std::move(dependencies));
    return resources;
}

void IDevice::DestroyBuffer(BufferHandle handle) { std::lock_guard<std::recursive_mutex> lock(m_resourceMutex); m_resources.erase(handle); }
void IDevice::DestroyTexture(TextureHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    if(!m_borrowedTextures.count(handle)) m_resources.erase(handle);
}
void IDevice::DestroyShader(ShaderHandle handle) { std::lock_guard<std::recursive_mutex> lock(m_resourceMutex); m_resources.erase(handle); }
void IDevice::DestroyPipeline(PipelineHandle handle) { std::lock_guard<std::recursive_mutex> lock(m_resourceMutex); m_resources.erase(handle); }
void IDevice::DestroyResourceSet(ResourceSetHandle handle) { std::lock_guard<std::recursive_mutex> lock(m_resourceMutex); m_resources.erase(handle); }

bool IDevice::UpdateBuffer(ICommandList& commands, BufferHandle buffer, uint32_t offset, const void* data, uint32_t size)
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    if(commands.m_owner != this || !commands.Track(buffer) || !data || !size
        || offset > buffer->GetDesc().size || size > buffer->GetDesc().size - offset)
    { commands.m_recordingFailed = true; return false; }
    const auto* first=static_cast<const uint8_t*>(data);
    std::vector<uint8_t> bytes(first,first+size);
    commands.RequireState(buffer,0,0,ResourceState::CopyDestination);
    commands.m_commands.push_back([this,buffer,offset,bytes=std::move(bytes)](ICommandList& native) {
        if(UpdateBufferNative(native,buffer,offset,bytes.data(),static_cast<uint32_t>(bytes.size())))return true;
        std::fprintf(stderr,"dyf::RHI [error]: Submit: UpdateBuffer failed (offset=%u, size=%zu, bufferSize=%u).\n",
            offset,bytes.size(),buffer->GetDesc().size);
        return false;
    });
    return true;
}

bool IDevice::UpdateTexture(ICommandList& commands, TextureHandle texture, uint32_t mip, uint32_t layer,
    const void* data, uint32_t size, uint32_t rowPitch, uint32_t slicePitch)
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    if(commands.m_owner != this || !commands.Track(texture) || !data || !size
        || mip >= texture->GetDesc().mipLevels || layer >= texture->GetDesc().depthOrArraySize)
    { commands.m_recordingFailed = true; return false; }
    const uint64_t row = uint64_t(std::max(1u, texture->GetDesc().width >> mip)) * FormatSize(texture->GetDesc().format);
    const uint64_t height = std::max(1u, texture->GetDesc().height >> mip);
    if(rowPitch < row || slicePitch < uint64_t(rowPitch) * height || size < uint64_t(rowPitch) * (height - 1) + row)
    {
        ReportDiagnostic(DiagnosticSeverity::Error,"UpdateTexture: rowPitch must cover one row, slicePitch must cover rowPitch*height, and dataSize must cover the last row's pixels. Trailing row padding is not required.");
        commands.m_recordingFailed = true; return false;
    }
    const auto* first=static_cast<const uint8_t*>(data);
    std::vector<uint8_t> bytes(first,first+size);
    commands.RequireState(texture,mip,layer,ResourceState::CopyDestination);
    commands.m_commands.push_back([this,texture,mip,layer,rowPitch,slicePitch,bytes=std::move(bytes)](ICommandList& native) {
        if(UpdateTextureNative(native,texture,mip,layer,bytes.data(),static_cast<uint32_t>(bytes.size()),rowPitch,slicePitch))return true;
        std::fprintf(stderr,"dyf::RHI [error]: Submit: UpdateTexture failed (mip=%u, layer=%u, size=%zu, rowPitch=%u, slicePitch=%u).\n",
            mip,layer,bytes.size(),rowPitch,slicePitch);
        return false;
    });
    return true;
}

bool IDevice::ReadTexture(TextureHandle texture, TextureReadback& result)
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    return Reference(texture) && ReadTextureNative(texture, result);
}
}
