#include "../Platform/Profiler/CollectorInternal.h"
#include <cstdio>
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/ICommandList.h"
#include "Validation.h"
#include "dyf/RHI/Readback.h"
#include "ThreadPool.h"
#include "dyf/RHI/Query.h"
#include "dyf/RHI/Backend/Observation.h"
#include "../Platform/Profiler/RhiProfiler.h"
#include <chrono>
#include <deque>
#include <new>
#include <stdexcept>
#include <thread>

namespace dyf::RHI
{
#if defined(DY_LIVE_PROFILING_ENABLED) || defined(DY_PROFILER_TRACING_ENABLED)
struct Backend::ReadbackTrace::State
{
    Platform::LiveId resource=0,submission=0;
    uint64_t bytes=0;
    std::optional<LiveRhiScope> scope;
    State(TextureHandle texture,uint64_t size) noexcept
        :resource(Platform::FindLiveObject(texture)),bytes(size) {}
};
Backend::ReadbackTrace::ReadbackTrace(TextureHandle texture,uint64_t bytes) noexcept
    :m_state(new(std::nothrow) State(texture,bytes)) {}
Backend::ReadbackTrace::~ReadbackTrace()=default;
void Backend::ReadbackTrace::Copy() noexcept
{
    if(!m_state)return;
    auto& state=*m_state;
    state.scope.emplace(Platform::LiveEventKind::CpuWork,Platform::LiveStage::NativeTranslate,"Readback: record texture copy");
    state.scope->event.resource=state.resource;
    state.scope->event.bytes=state.bytes;state.scope->event.operations=1;
    state.scope->event.outcome=Platform::LiveOutcome::Failed;
}
void Backend::ReadbackTrace::Submit() noexcept
{
    if(!m_state)return;
    auto& state=*m_state;
    state.submission=Platform::IsLiveCollectorEnabled()?Platform::AllocateLiveId():0;
    state.scope.emplace(Platform::LiveEventKind::CpuWork,Platform::LiveStage::Submit,"Readback: native submission");
    state.scope->event.resource=state.resource;state.scope->event.submission=state.submission;
    state.scope->event.operations=1;state.scope->event.outcome=Platform::LiveOutcome::Failed;
}
void Backend::ReadbackTrace::Wait() noexcept
{
    if(!m_state)return;
    auto& state=*m_state;
    state.scope.emplace(Platform::LiveEventKind::Wait,Platform::LiveStage::Submit,"Readback: native wait");
    state.scope->event.resource=state.resource;state.scope->event.submission=state.submission;
    state.scope->event.operations=1;state.scope->event.outcome=Platform::LiveOutcome::Failed;
}
void Backend::ReadbackTrace::Finish(bool success) noexcept
{
    if(!m_state || !m_state->scope)return;
    auto& state=*m_state;
    auto& event=state.scope->event;
    const bool wait=event.kind==Platform::LiveEventKind::Wait;
    const bool submit=event.stage==Platform::LiveStage::Submit&&!wait;
    event.outcome=!success?Platform::LiveOutcome::Failed:
        wait?Platform::LiveOutcome::Completed:submit?Platform::LiveOutcome::Submitted:Platform::LiveOutcome::Observed;
    state.scope.reset();
    if(success&&submit&&state.submission)
        Platform::SetLiveGpuCoverage(Platform::GetLiveContext().frame,false);
    if(success&&wait)RecordLiveReadbackCompletion(state.submission,state.resource);
}
#endif
#ifdef DY_LIVE_PROFILING_ENABLED
void Backend::ObserveUploadPage(uint64_t bytes,bool reused) noexcept
{
    const auto context=Platform::GetLiveContext();
    if(!context.frame.frame||!Platform::IsLiveCollectorEnabled())return;
    Platform::LiveEvent event;event.id=Platform::AllocateLiveId();event.context=context;
    event.kind=Platform::LiveEventKind::UploadMemory;event.stage=Platform::LiveStage::NativeTranslate;
    event.arguments[0]=reused?1:0;event.bytes=bytes;event.operations=1;
    Platform::RecordLiveEvent(event);
}
#endif

ResourceSet::ResourceSet(const ResourceSetDesc& desc) : m_pipeline(desc.pipeline)
{
    if(!m_pipeline || (desc.bindingCount && !desc.bindings))
        throw std::runtime_error("Resource-set bindings or pipeline are missing.");
    if(desc.bindingCount)m_bindings.assign(desc.bindings,desc.bindings+desc.bindingCount);
    m_stateRequirements.reserve(m_bindings.size());
    // Layouts, bindings and texture dimensions are immutable. Resolve their
    // requirements once; recording still checks the current state after barriers.
    for(const auto& binding:m_bindings)
    {
        const auto* layout=FindLayoutBinding(m_pipeline->GetLayout(),binding.binding);
        if(!layout)throw std::runtime_error("Resource-set binding is absent from the pipeline layout.");
        StateRequirement requirement;
        requirement.buffer=binding.buffer;requirement.texture=binding.texture;
        if(binding.buffer)
            requirement.state=layout->type==ResourceBindingType::ConstantBuffer?ResourceState::ConstantBuffer:
                layout->type==ResourceBindingType::ReadWriteStorageBuffer?ResourceState::UnorderedAccess:ResourceState::ShaderResource;
        else
        {
            requirement.state=layout->type==ResourceBindingType::StorageTexture?ResourceState::UnorderedAccess:ResourceState::ShaderResource;
            if(!ResolveBindingSubresources(binding.texture,binding,layout->type,requirement.firstMip,
                requirement.mipCount,requirement.firstLayer,requirement.layerCount))
                throw std::runtime_error("Resource-set texture subresources are invalid.");
        }
        m_stateRequirements.push_back(requirement);
    }
}

Texture::~Texture()
{
    Platform::RetireLiveObject(Platform::FindLiveObject(this));
}

namespace
{
const char* LiveShaderStage(ShaderStage stage) noexcept
{
    switch(stage)
    {case ShaderStage::Vertex:return "Vertex";case ShaderStage::Fragment:return "Fragment";
     case ShaderStage::Compute:return "Compute";case ShaderStage::Hull:return "Hull";
     case ShaderStage::Domain:return "Domain";case ShaderStage::Mesh:return "Mesh";default:return "Unknown";}
}
const char* LiveTopology(PrimitiveTopology topology) noexcept
{
    switch(topology)
    {case PrimitiveTopology::PointList:return "PointList";case PrimitiveTopology::LineList:return "LineList";
     case PrimitiveTopology::TriangleList:return "TriangleList";case PrimitiveTopology::TriangleStrip:return "TriangleStrip";
     case PrimitiveTopology::PatchList:return "PatchList";default:return "Undefined";}
}
}
struct IDevice::RecordingState
{
    struct RetiredResource
    {
        void* object;
        void (*destroy)(IDevice&,void*);
        Platform::LiveId liveId;
        std::vector<std::shared_ptr<void>> dependencies;
        RetiredResource* next=nullptr;
    };
    static_assert(std::atomic<RetiredResource*>::is_always_lock_free,
        "Resource retirement requires lock-free pointer publication.");
    const std::thread::id ownerThread=std::this_thread::get_id();
    std::atomic<RetiredResource*> retired{nullptr};
    bool draining=false;
    static constexpr size_t CommandLimit=256;
    static constexpr uint64_t PayloadLimit=64u*1024u*1024u;
    struct DeleteCommand
    {
        void operator()(ICommandList* commands) const {delete commands;}
    };
    using CommandOwner=std::unique_ptr<ICommandList,DeleteCommand>;
    struct CachedCommand
    {
        CommandOwner commands;
        uint64_t bytes=0;
    };
    uint32_t threadCount=0;
    std::shared_ptr<ThreadPool> pool;
    // FIFO keeps large geometry payloads near their previous pass positions.
    // Entries may still be in flight; Acquire checks completion before reset.
    std::deque<CachedCommand> commands;
    uint64_t commandBytes=0;
    uint64_t completedSubmission=0;
    bool reportedReuse=false;
};

thread_local const IDevice* IDevice::s_recordingDevice=nullptr;
thread_local const IDevice::RecordingResources* IDevice::s_recordingResources=nullptr;

IDevice::RecordingResourceScope::RecordingResourceScope(
    const IDevice& device,const RecordingResources* resources) noexcept
    :previousDevice(s_recordingDevice),previousResources(s_recordingResources)
{
    s_recordingDevice=&device;
    s_recordingResources=resources;
}
IDevice::RecordingResourceScope::~RecordingResourceScope()
{
    s_recordingDevice=previousDevice;
    s_recordingResources=previousResources;
}

std::shared_ptr<const IDevice::RecordingResources> IDevice::CaptureRecordingResources()
{
    RequireOwnerThread();
    if(!m_recordingResources)
    {
        auto resources=std::make_shared<RecordingResources>();
        resources->reserve(m_resources.size());
        for(const auto& entry:m_resources)
            if(!m_borrowedTextures.count(entry.first))resources->emplace(entry);
        m_recordingResources=std::move(resources);
    }
    return m_recordingResources;
}

void IDevice::UnregisterResource(const void* handle)
{
    const auto resource=m_resources.find(handle);
    if(resource==m_resources.end())return;
    resource->second->registered.store(false,std::memory_order_release);
    if(!m_borrowedTextures.count(handle))m_recordingResources.reset();
    m_resources.erase(resource);
}

IDevice::IDevice() : m_recording(std::make_shared<RecordingState>())
{
    GetLiveState();
}

void IDevice::RequireOwnerThread() const
{
    if(!IsOwnerThread())
        throw std::logic_error("Device mutation, submission and retirement require its creating thread; workers may record independent command lists.");
    const_cast<IDevice*>(this)->DrainRetiredResources();
}

bool IDevice::IsOwnerThread() const noexcept
{
    return m_recording->ownerThread==std::this_thread::get_id();
}

void IDevice::DrainRetiredResources() noexcept
{
    if(!IsOwnerThread() || m_recording->draining ||
        !m_recording->retired.load(std::memory_order_acquire)) return;
    m_recording->draining=true;
    // Producers only publish preallocated nodes. Native destruction and dependency
    // release belong to the owner, after command/submission references expire.
    while(auto* pending=m_recording->retired.exchange(nullptr,std::memory_order_acquire))
    {
        RecordingState::RetiredResource* ordered=nullptr;
        while(pending)
        {
            auto* next=pending->next;
            pending->next=ordered;ordered=pending;pending=next;
        }
        while(ordered)
        {
            auto* retired=ordered;ordered=ordered->next;
            const uintptr_t key=reinterpret_cast<uintptr_t>(retired->object);
            const auto first=m_resourceStates.lower_bound({key,0,0});
            auto last=first;
            while(last!=m_resourceStates.end() && std::get<0>(last->first)==key)++last;
            m_resourceStates.erase(first,last);
            if(m_nativeResourcesAlive)
            {
                retired->destroy(*this,retired->object);
                Platform::RetireLiveObject(retired->liveId);
            }
            else
            {
                Platform::LiveEvent event;event.context=Platform::GetLiveContext();event.resource=retired->liveId;
                event.kind=Platform::LiveEventKind::CpuWork;event.stage=Platform::LiveStage::Data;
                event.outcome=Platform::LiveOutcome::Unknown;Platform::RecordLiveEvent(event);
            }
            delete retired; // Releasing dependencies may publish further nodes.
        }
    }
    PublishLiveResources();
    m_recording->draining=false;
}
IDevice::~IDevice()
{
    StopLiveProfiling();
    if(m_live)
    {
        Platform::RetireLiveObject(m_live->recordingSource);
        Platform::RetireLiveObject(m_live->device);
    }
}
UploadMemoryStatistics IDevice::GetUploadMemoryStatistics() const
{
    return GetUploadMemoryStatisticsNative();
}
UploadMemoryStatistics IDevice::GetUploadMemoryStatisticsNative() const { return {}; }

void IDevice::RecordParallel(uint32_t count, uint32_t threadCount,
    const std::function<void(uint32_t)>& record)
{
    if(!count) return;
    // A nested recorder already holds this device's immutable resource snapshot.
    // It records locally without entering owner-only preparation.
    if(s_recordingDevice == this)
    {
        for(uint32_t i = 0; i < count; ++i) record(i);
        return;
    }
    const auto resources = CaptureRecordingResources();
    const auto pinnedRecord = [&](uint32_t index) {
        RecordingResourceScope scope(*this, resources.get());
        record(index);
    };
    if(count <= 1 || threadCount == 1)
    {
        for(uint32_t i = 0; i < count; ++i) pinnedRecord(i);
        return;
    }
    std::shared_ptr<ThreadPool> recording;
    {
        RequireOwnerThread();
        if(!m_recording) m_recording=std::make_shared<RecordingState>();
        if(!m_recording->pool || m_recording->threadCount!=threadCount)
        {
            auto pool=std::make_shared<ThreadPool>(threadCount);
            m_recording->pool=std::move(pool);m_recording->threadCount=threadCount;
        }
        recording=m_recording->pool;
    }
    auto& pool = *recording;
    if(pool.GetThreadCount() <= 1 || pool.IsWorkerThread())
    {
        for(uint32_t i = 0; i < count; ++i) pinnedRecord(i);
        return;
    }
    pool.ParallelFor(count, pinnedRecord);
}

void IDevice::ReportDiagnostic(DiagnosticSeverity severity, const char* message) const
{
    const char* level = severity == DiagnosticSeverity::Error ? "Error" :
        severity == DiagnosticSeverity::Warning ? "Warning" : "Info";
    std::fprintf(stderr, "[RHI][%s] %s\n", level, message ? message : "");
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
    RequireOwnerThread();
    if(!Reference(desc.vertexShader) || (desc.hullShader && !Reference(desc.hullShader)) ||
        (desc.domainShader && !Reference(desc.domainShader)) ||
        (desc.fragmentShader && !Reference(desc.fragmentShader))) return false;
    if(!Supports(desc.layout) || !ValidateGraphicsPipelineDesc(desc)) return false;
    if(desc.hullShader && (!Supports(Feature::Tessellation) ||
        desc.patchControlPoints > GetLimit(Limit::TessellationPatchControlPoints))) return false;
    if(desc.raster.fillMode == FillMode::Wireframe && !Supports(Feature::Wireframe)) return false;
    if(desc.raster.depthBiasClamp != 0 && !Supports(Feature::DepthBiasClamp)) return false;
    if(std::trunc(desc.raster.depthBiasConstant) != desc.raster.depthBiasConstant &&
        !Supports(Feature::FractionalDepthBias)) return false;
    return SupportsGraphicsPipelineNative(desc);
}

bool IDevice::Supports(const MeshPipelineDesc& desc) const
{
    RequireOwnerThread();
    if(!Supports(Feature::MeshShader)) return false;
    if(!Reference(desc.meshShader) || (desc.fragmentShader && !Reference(desc.fragmentShader))) return false;
    if(!Supports(desc.layout) || !ValidateMeshPipelineDesc(desc)) return false;
    if(desc.raster.fillMode == FillMode::Wireframe && !Supports(Feature::Wireframe)) return false;
    if(desc.raster.depthBiasClamp != 0 && !Supports(Feature::DepthBiasClamp)) return false;
    if(std::trunc(desc.raster.depthBiasConstant) != desc.raster.depthBiasConstant &&
        !Supports(Feature::FractionalDepthBias)) return false;
    return SupportsMeshPipelineNative(desc);
}

TimestampQueryHandle IDevice::CreateTimestampQuery(const TimestampQueryDesc& desc)
{
    RequireOwnerThread();
    if(!desc.count || !Supports(Feature::TimestampQuery)) return nullptr;
    auto* query=CreateTimestampQueryNative(desc);
    Track(query,[](IDevice& device,void* object){device.DestroyTimestampQueryNative(static_cast<TimestampQueryHandle>(object));},
        {},Platform::LiveObjectKind::Query,desc.count,uint64_t(desc.count)*sizeof(uint64_t));
    return query;
}
void IDevice::DestroyTimestampQuery(TimestampQueryHandle query)
{
    RequireOwnerThread();
    RecordLiveRelease(query);
    UnregisterResource(query);
}
bool IDevice::ReadTimestamps(TimestampQueryHandle query,uint32_t first,uint32_t count,uint64_t* ticks)
{return ReadTimestampsInternal(query,first,count,ticks,true);}

bool IDevice::ReadTimestampsInternal(TimestampQueryHandle query,uint32_t first,uint32_t count,uint64_t* ticks,bool observe)
{
    RequireOwnerThread();
    auto* live=observe?GetLiveState():nullptr;
    Platform::LiveFrameScope frame(live?live->device:0,live?live->device:0);
    frame.SetOutcome(Platform::LiveOutcome::Failed);
    LiveRhiScope scope(Platform::LiveEventKind::CpuWork,Platform::LiveStage::Output,"Read timestamps",observe);
    scope.event.resource=Platform::FindLiveObject(query);scope.event.arguments={first,count};scope.event.operations=1;
    scope.event.outcome=Platform::LiveOutcome::Failed;
    if(!Reference(query) || !ticks || !count || first>=query->GetCount() || count>query->GetCount()-first)
    {frame.SetOutcome(Platform::LiveOutcome::Failed);return false;}
    const bool ready=ReadTimestampsNative(query,first,count,ticks);
    scope.event.outcome=ready?Platform::LiveOutcome::Completed:Platform::LiveOutcome::Unknown;
    if(ready)scope.event.bytes=uint64_t(count)*sizeof(uint64_t);
    frame.SetOutcome(scope.event.outcome);return ready;
}
bool IDevice::GetTimestampCalibration(TimestampCalibration& calibration) const
{
    RequireOwnerThread();
    calibration = {};
    TimestampCalibration sample;
    if(!Supports(Feature::TimestampCalibration) || !GetTimestampCalibrationNative(sample) ||
        sample.cpuDomain == CpuTimestampDomain::Unsupported || !sample.cpuFrequency ||
        !sample.gpuValidBits || sample.gpuValidBits > 64 ||
        !std::isfinite(sample.gpuPeriodNanoseconds) || sample.gpuPeriodNanoseconds <= 0) return false;
    calibration = sample;
    return true;
}

void IDevice::InvalidateBackBuffers()
{
    ++m_imageGeneration;
    for(const auto* texture : m_borrowedTextures)
    {
        UnregisterResource(texture);
        m_resourceStates.erase({reinterpret_cast<uintptr_t>(texture),0,0});
    }
    m_borrowedTextures.clear();
}

bool IDevice::IsLost() const
{
    RequireOwnerThread();
    return IsLostNative();
}

bool IDevice::WaitIdle()
{
    RequireOwnerThread();
    auto* live=GetLiveState();
    Platform::LiveFrameScope frame(live?live->device:0,live?live->device:0);
    LiveRhiScope scope(Platform::LiveEventKind::Wait,Platform::LiveStage::Submit,"WaitIdle");
    frame.SetOutcome(Platform::LiveOutcome::Failed);scope.event.outcome=Platform::LiveOutcome::Failed;scope.event.operations=1;
    const bool complete=WaitIdleNative();
    DrainRetiredResources();
    PumpLiveWork();
    scope.event.operations=1;scope.event.outcome=complete?Platform::LiveOutcome::Completed:Platform::LiveOutcome::Failed;
    frame.SetOutcome(scope.event.outcome);
    return complete;
}

bool IDevice::TryBeginSwapchainRetirement()
{
    RequireOwnerThread();
    if(m_swapchainRetirementPending) return false;
    m_swapchainRetirementPending = true;
    return true;
}

void IDevice::CompleteSwapchainRetirement()
{
    RequireOwnerThread();
    m_swapchainRetirementPending = false;
}

bool IDevice::CreateSwapchain(const SwapchainDesc& desc)
{
    RequireOwnerThread();
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
            if(reference != m_resources.end() && reference->second->owner.use_count() != 1) return false;
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
    RequireOwnerThread();
    if(m_frameActive) return true;
    PumpLiveWork();
    auto* live=GetLiveState();
    if(live)
    {live->frame=Platform::BeginLiveFrame(live->device,live->device);live->hasFrame=live->frame.key.frame!=0;PrepareLiveControls(live->frame.key);}
    LiveRhiScope scope(Platform::LiveEventKind::Output,Platform::LiveStage::Output,"BeginFrame / acquire");
    scope.event.operations=1;
    InvalidateBackBuffers();
    m_frameActive = BeginFrameNative();
    if(live)
    {
        scope.event.outcome=m_frameActive?Platform::LiveOutcome::Observed:
            (m_hasSwapchain&&!IsLostNative()?Platform::LiveOutcome::Skipped:Platform::LiveOutcome::Failed);
        if(scope.event.outcome==Platform::LiveOutcome::Skipped)
        {scope.event.kind=Platform::LiveEventKind::Wait;scope.event.id=scope.event.context.frame.frame;}
        scope.Finish();
        if(!m_frameActive&&live->hasFrame)
        {Platform::EndLiveFrame(live->frame,scope.event.outcome);live->hasFrame=false;}
    }
    return m_frameActive;
}

bool IDevice::Present()
{
    RequireOwnerThread();
    if(!m_frameActive) {std::fprintf(stderr, "[RHI] dyf::RHI: Present requires an active frame.\n");return false;}
    auto* backBuffer = GetBackBuffer();
    const auto state = m_resourceStates.find({reinterpret_cast<uintptr_t>(backBuffer),0,0});
    if(!backBuffer || state == m_resourceStates.end() || state->second != ResourceState::Present)
    {std::fprintf(stderr, "[RHI] dyf::RHI: Submit the backbuffer transition to Present before presenting.\n");return false;}
    LiveRhiScope scope(Platform::LiveEventKind::Output,Platform::LiveStage::Output,"Present");
    scope.event.commandKind=Platform::LiveCommandKind::Present;
    scope.event.resource=Platform::FindLiveObject(backBuffer);scope.event.operations=1;
    if(m_live)scope.event.submission=m_live->backBufferSubmission;
    const bool presented = PresentNative();
    m_frameActive = false;
    scope.event.outcome=presented&&!IsLostNative()?Platform::LiveOutcome::Completed:Platform::LiveOutcome::Failed;
    scope.Finish();
    if(m_live&&m_live->hasFrame)
    {Platform::EndLiveFrame(m_live->frame,scope.event.outcome);m_live->hasFrame=false;}
    if(!presented || IsLostNative()) {std::fprintf(stderr, "[RHI] dyf::RHI: Presentation failed.\n");return false;}
    return true;
}


std::shared_ptr<void> IDevice::Reference(const void* handle) const
{
    RequireOwnerThread();
    const auto found = m_resources.find(handle);
    return found == m_resources.end() ? nullptr : found->second->owner;
}

void IDevice::Track(void* handle,void (*destroy)(IDevice&,void*),std::vector<std::shared_ptr<void>> dependencies)
{
    Track(handle,destroy,std::move(dependencies),Platform::LiveObjectKind::Entity);
}
void IDevice::Track(void* handle, void (*destroy)(IDevice&, void*),
    std::vector<std::shared_ptr<void>> dependencies,Platform::LiveObjectKind kind,uint64_t elements,uint64_t bytes,const char* label)
{
    if(!handle) return;
    Platform::LiveId liveId=0;
    if(auto* live=GetLiveState())liveId=Platform::RegisterLiveObject(handle,
        {kind,live->device,Platform::GetLiveContext().owner,elements,bytes,label});
    RecordingState::RetiredResource* retired=nullptr;
    try {retired=new RecordingState::RetiredResource{handle,destroy,liveId,std::move(dependencies)};}
    catch(...)
    {
        if(m_nativeResourcesAlive) {destroy(*this,handle);Platform::RetireLiveObject(liveId);}
        throw;
    }
    // shared_ptr invokes this deleter even if its control-block allocation fails.
    auto reference = std::shared_ptr<void>(handle,[state=m_recording.get(),retired](void*) noexcept {
        auto* head=state->retired.load(std::memory_order_relaxed);
        do {retired->next=head;}
        while(!state->retired.compare_exchange_weak(head,retired,
            std::memory_order_release,std::memory_order_relaxed));
    });
    m_resources.emplace(handle,std::make_shared<ResourceRecord>(std::move(reference)));
    m_recordingResources.reset();
    PublishLiveResources();
}

ResourceAllocationCounters IDevice::GetResourceAllocationCounters() const
{
    RequireOwnerThread();
    return m_allocationCounters;
}

void IDevice::AbandonResources()
{
    m_nativeResourcesAlive = false;
    ReleaseResources();
}

void IDevice::ReleaseResources()
{
    StopLiveProfiling();
    RequireOwnerThread();
    // Native destructors call this while their resource operations are still valid.
    // Cached wrappers are not user handles. Destroy them while this device is
    // alive, before walking the set their destructors remove themselves from.
    if(m_recording) {m_recording->commands.clear();m_recording->commandBytes=0;}
    std::vector<std::shared_ptr<void>> references;
    std::vector<std::shared_ptr<const ICommandList::ReferenceSnapshot>> snapshots;
    for(auto* commands : m_recordedCommands)
    {
        for(auto& reference : commands->m_references) references.push_back(std::move(reference));
        commands->m_references.clear();
        commands->m_referenceHandles.clear();
        commands->m_liveDeferred->ClearTrackedHandles();
        if(commands->m_sharedReferences)snapshots.push_back(std::move(commands->m_sharedReferences));
        commands->m_owner = nullptr;
    }
    m_recordedCommands.clear();
    for(auto* commands : m_userCommands) delete commands;
    m_userCommands.clear();
    for(const auto& resource:m_resources)
        resource.second->registered.store(false,std::memory_order_release);
    m_recordingResources.reset();
    m_resources.clear();
    references.clear();
    snapshots.clear();
    DrainRetiredResources();
    m_resourceStates.clear();
    m_borrowedTextures.clear();
}

ICommandList* IDevice::AcquireCommandList()
{
    RequireOwnerThread();
    if(!m_recording) m_recording=std::make_shared<RecordingState>();
    const auto rollback = [this](ICommandList* value) {
        m_userCommands.erase(value);
        delete value;
    };
    ICommandList* cached=nullptr;
    if(!m_recording->commands.empty())
    {
        const auto findReady=[&] {
            auto entry=m_recording->commands.begin();
            for(;entry!=m_recording->commands.end();++entry)
                if(entry->commands->m_completion<=m_recording->completedSubmission)break;
            return entry;
        };
        auto entry=findReady();
        if(entry==m_recording->commands.end())
        {
            // A proven completion stays valid for this device. Poll again only
            // when the cache has no entry covered by that monotonic serial.
            const uint64_t completed=GetCompletedSubmissionNative();
            if(completed>m_recording->completedSubmission)m_recording->completedSubmission=completed;
            entry=findReady();
        }
        if(entry!=m_recording->commands.end())
        {
            cached=entry->commands.release();
            m_recording->commandBytes-=entry->bytes;
            m_recording->commands.erase(entry);
        }
    }
    std::unique_ptr<ICommandList, decltype(rollback)> owner(cached?cached:ICommandList::CreateRecorded(), rollback);
    auto* commands = owner.get();
    commands->m_owner = this;
    if(auto* live=GetLiveState())
    {
        commands->m_liveDeferred->m_livePosition={Platform::RegisterLiveObject(commands,
            {Platform::LiveObjectKind::CommandList,live->device,Platform::GetLiveContext().owner,0,0,"Command list"}),cached?0u:1u,0};
    }
    m_userCommands.insert(commands);
    m_recordedCommands.insert(commands);
    if(cached)
    {
        // Reset is still the single initialization path, including state maps,
        // references, frame generation and profiling contexts. Native lists are
        // never put in this cache and retain their existing fence-owned pools.
        commands->m_uploadPageReuse=commands->m_uploadSuballocation=true;
        commands->m_liveDeferred->m_liveReplayQuery=nullptr;commands->m_liveDeferred->m_liveFirstQuery=0;
        // Completion was checked by this same device owner above; do not poll
        // the native fence a second time inside ResetCommandList.
        commands->m_completion=0;
        if(!ResetCommandList(commands)) return nullptr;
        if(!m_recording->reportedReuse)
        {
            std::fprintf(stderr,"[RHI] Reusing completed recorded command-list storage (FIFO; limit=%zu, payload=%llu bytes).\n",
                RecordingState::CommandLimit,static_cast<unsigned long long>(RecordingState::PayloadLimit));
            m_recording->reportedReuse=true;
        }
    }
    else
    {
        BeginLiveRecording(*commands);
        CaptureRecordingLookup(*commands);
    }
    return owner.release();
}

TextureHandle IDevice::GetBackBuffer()
{
    RequireOwnerThread();
    auto* texture = GetBackBufferNative();
    if(texture && !Reference(texture))
    {
        m_borrowedTextures.insert(texture);
        m_resources.emplace(texture,std::make_shared<ResourceRecord>(
            std::shared_ptr<void>(texture,[](void*) {})));
        if(auto* live=GetLiveState())Platform::RegisterLiveObject(texture,
            {Platform::LiveObjectKind::Texture,live->device,live->device,0,0,"Swapchain image (borrowed)"});
        m_resourceStates[{reinterpret_cast<uintptr_t>(texture),0,0}]=ResourceState::Present;
    }
    return texture;
}



bool IDevice::Submit(const SubmitDesc& desc, FenceHandle& completion)
{
    completion={};
    RequireOwnerThread();
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
    RequireOwnerThread();
    const bool complete=completion.m_device==this && completion.m_value && completion.m_value<=GetCompletedSubmissionNative();
    DrainRetiredResources();
    PumpLiveWork();
    return complete;
}
bool IDevice::Wait(FenceHandle completion,uint64_t timeoutNanoseconds)
{
    auto* live=GetLiveState();
    Platform::LiveFrameScope frame(live?live->device:0,live?live->device:0);
    LiveRhiScope scope(Platform::LiveEventKind::Wait,Platform::LiveStage::Submit,"Wait fence");
    frame.SetOutcome(Platform::LiveOutcome::Failed);
    scope.event.operations=1;scope.event.submission=GetLiveSubmissionId(completion);
    scope.event.outcome=Platform::LiveOutcome::Failed;
    if(completion.m_device!=this || !completion.m_value) return false;
    const auto start=std::chrono::steady_clock::now();
    do
    {
        if(IsComplete(completion)) {scope.event.outcome=Platform::LiveOutcome::Completed;frame.SetOutcome(scope.event.outcome);return true;}
        if(IsLost()) return false;
        if(timeoutNanoseconds==0) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    } while(timeoutNanoseconds==UINT64_MAX || static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now()-start).count())<timeoutNanoseconds);
    return false;
}

bool IDevice::ResetCommandList(ICommandList* commands)
{
    RequireOwnerThread();
    if(!commands || !m_userCommands.count(commands) ||
        (commands->m_completion && commands->m_completion>GetCompletedSubmissionNative())) return false;
    commands->FinishLiveRecording(commands->m_recordingFailed?Platform::LiveOutcome::Failed:Platform::LiveOutcome::Discarded,true);
    if(commands->m_preparedNative)
    {
        DiscardCommandListNative(commands->m_preparedNative);
        commands->m_preparedNative = nullptr;
    }
    DiscardLivePrepared(commands,Platform::LiveOutcome::Discarded);
    commands->m_commands.clear();
    commands->m_commandBytes.clear();
    commands->m_stateOperations.clear();
    commands->m_recordedStates.clear();
    commands->m_requiredResourceSets.clear();
    commands->m_liveDeferred->ClearRecentStates();
    commands->m_liveDeferred->inlineConstants.Reset(0);
#ifdef DY_LIVE_PROFILING_ENABLED
    commands->m_liveDeferred->objectIds.clear();
#endif
    commands->m_references.clear();
    commands->m_referenceHandles.clear();
    commands->m_sharedReferences.reset();
    commands->m_liveDeferred->ClearTrackedHandles();
    commands->m_recordingClosed=commands->m_recordingFailed=false;
    commands->m_debugEventDepth=0;
    commands->m_rendering=commands->m_viewport=commands->m_scissor=false;
    commands->m_stencilAttachment=commands->m_stencilReference=false;
    commands->m_pipeline=nullptr;
    commands->m_indexBuffer=nullptr;
    commands->m_indexFormat=Format::Unknown;commands->m_indexOffset=0;
    commands->m_resourceSet=nullptr;
    commands->m_vertexBuffers.clear();
    commands->m_colorFormats.clear();
    commands->m_depthStencilFormat=Format::Unknown;
    commands->m_depthStencilState=ResourceState::Undefined;
    commands->m_completion=commands->m_imageGeneration=0;
    if(commands->m_liveDeferred->m_livePosition.commandList)++commands->m_liveDeferred->m_livePosition.generation;
    commands->m_liveDeferred->m_livePosition.index=0;
    commands->m_liveDeferred->m_liveContext={};commands->m_liveDeferred->m_liveExecutionContext={};commands->m_liveDeferred->m_liveRecordFrame={};
    commands->m_liveDeferred->m_liveInExecution=commands->m_liveDeferred->m_liveSubmitted=commands->m_liveDeferred->m_liveTerminal=false;
    commands->m_liveDeferred->m_liveCommandBeginNs=0;commands->m_liveDeferred->m_livePreviousEvent=0;commands->DiscardLiveDeferred();
    BeginLiveRecording(*commands);
    CaptureRecordingLookup(*commands);
    return true;
}
void IDevice::DestroyCommandList(ICommandList* commands)
{
    RequireOwnerThread();
    if(!commands || !m_userCommands.count(commands)) return;
    // Empty closed lists need no GPU retirement and may also reuse their storage.
    // Failed/open or nonempty unsubmitted lists retain the destruction path.
    if(m_recording && commands->m_recordingClosed && !commands->m_recordingFailed &&
        (commands->m_completion || commands->m_commands.empty()) && !commands->m_preparedNative &&
        m_recording->commands.size()<RecordingState::CommandLimit)
    {
        uint64_t bytes=0;
        const auto account=[&](size_t count,size_t stride) {
            if(count>(RecordingState::PayloadLimit-bytes)/stride) return false;
            bytes+=count*stride;return true;
        };
        if(account(commands->m_commandBytes.capacity(),sizeof(uint8_t)) &&
            account(commands->m_commands.capacity(),sizeof(decltype(commands->m_commands)::value_type)) &&
            account(commands->m_stateOperations.capacity(),sizeof(ICommandList::StateOperation)) &&
            account(commands->m_references.capacity(),sizeof(std::shared_ptr<void>)) &&
            account(commands->m_colorFormats.capacity(),sizeof(Format)) &&
            bytes<=RecordingState::PayloadLimit-m_recording->commandBytes)
        {
            // Release the same CPU owners/closures as deletion, without freeing
            // their payload capacity. Native submissions already own GPU leases.
            commands->FinishLiveRecording(Platform::LiveOutcome::Discarded,true);
            DiscardLivePrepared(commands,Platform::LiveOutcome::Discarded);
            commands->DiscardLiveDeferred();
            Platform::RetireLiveObject(commands->m_liveDeferred->m_livePosition.commandList);
            commands->m_liveDeferred->m_livePosition={};
            commands->m_commands.clear();commands->m_commandBytes.clear();
            commands->m_stateOperations.clear();commands->m_recordedStates.clear();commands->m_requiredResourceSets.clear();
            commands->m_liveDeferred->ClearRecentStates();
            commands->m_references.clear();commands->m_referenceHandles.clear();commands->m_sharedReferences.reset();
            commands->m_liveDeferred->ClearTrackedHandles();
            commands->m_vertexBuffers.clear();commands->m_colorFormats.clear();
#ifdef DY_LIVE_PROFILING_ENABLED
            commands->m_liveDeferred->objectIds.clear();
#endif
            m_userCommands.erase(commands);
            try
            {
                m_recording->commands.push_back({RecordingState::CommandOwner(commands),bytes});
                m_recording->commandBytes+=bytes;
                return;
            }
            catch(...)
            {
                // The temporary unique_ptr has destroyed the uncached wrapper.
                return;
            }
        }
    }
    m_userCommands.erase(commands);delete commands;
}

bool IDevice::PrepareCommandLists(ICommandList* const* commands, uint32_t count, uint32_t threadCount)
{
    if(!commands || !count) return false;
    struct Work { ICommandList* source; ICommandList* native; };
    std::vector<Work> work;
    work.reserve(count);
    const auto discard = [&] {
        RequireOwnerThread();
        for(const auto& item : work)
        {DiscardCommandListNative(item.native);DiscardLivePrepared(item.source,Platform::LiveOutcome::Failed);}
    };
    try
    {
        {
            RequireOwnerThread();
            const uint64_t completed = GetCompletedSubmissionNative();
            for(uint32_t i = 0; i < count; ++i)
            {
                if(!commands[i] || !m_userCommands.count(commands[i]) ||
                    !commands[i]->m_recordingClosed || commands[i]->m_recordingFailed ||
                    commands[i]->m_completion > completed ||
                    (commands[i]->m_imageGeneration && !m_frameActive) ||
                    (commands[i]->m_imageGeneration && commands[i]->m_imageGeneration != m_imageGeneration)) return false;
                for(uint32_t j = 0; j < i; ++j) if(commands[i] == commands[j]) return false;
            }
            // Native pools/allocators are allocated by the device owner. Each list
            // has its own recording storage; workers never mutate the ownership sets.
            for(uint32_t i = 0; i < count; ++i)
            {
                if(commands[i]->m_preparedNative) continue;
                auto* native = AcquireCommandListNative(commands[i]->m_uploadPageReuse,commands[i]->m_uploadSuballocation);
                if(!native) { discard(); return false; }
                work.push_back({commands[i], native});
                native->m_owner = this;
                m_recordedCommands.insert(native);
                native->m_references = commands[i]->m_references;
                // Native replay calls *Native directly; only the shared owners
                // are needed here, not the logical recorder's Track lookup index.
                native->m_sharedReferences = commands[i]->m_sharedReferences;
                const auto slot=ReserveLiveQuery(commands[i]);
                if(slot!=UINT32_MAX&&m_live)
                {
                    try
                    {
                        native->m_references.push_back(Reference(m_live->query));
                        native->m_liveDeferred->m_liveReplayQuery=m_live->query;native->m_liveDeferred->m_liveFirstQuery=slot*2;
                    }
                    catch(...){DiscardLivePrepared(commands[i],Platform::LiveOutcome::Skipped);}
                }
            }
        }
        if(work.empty())return true;
        LiveExecutionScope execution(*this,commands,count);
        const auto callerContext=Platform::GetLiveContext();
        std::vector<uint8_t> recorded(work.size(), 0);
        const auto record = [&](uint32_t i) {
            Platform::LiveContextScope context(work[i].source->m_liveDeferred->m_liveExecutionContext.frame.frame?
                work[i].source->m_liveDeferred->m_liveExecutionContext:callerContext);
            LiveRhiScope scope(Platform::LiveEventKind::NativeTranslation,Platform::LiveStage::NativeTranslate,"Native replay");
            scope.event.command=work[i].source->m_liveDeferred->m_livePosition;scope.event.operations=1;
            scope.event.outcome=Platform::LiveOutcome::Failed;
            recorded[i] = work[i].native->ReplayNative(work[i].source->m_commands);
            scope.event.outcome=recorded[i]?Platform::LiveOutcome::Observed:Platform::LiveOutcome::Failed;
        };
        RecordParallel(static_cast<uint32_t>(work.size()), threadCount, record);
        for(auto success : recorded)
            if(!success)
            {
                ReportDiagnostic(DiagnosticSeverity::Error, "PrepareCommandLists: native recording failed.");
                discard();
                return false;
            }
        for(const auto& item : work) item.source->m_preparedNative = item.native;
        execution.outcome=Platform::LiveOutcome::Observed;
        return true;
    }
    catch(...) { discard(); throw; }
}

bool IDevice::Submit(ICommandList** commands, uint32_t count)
{
    RequireOwnerThread();
    LiveExecutionScope execution(*this,commands,count);
    Platform::LiveContextScope context(commands&&count&&commands[0]&&m_userCommands.count(commands[0])?
        commands[0]->m_liveDeferred->m_liveExecutionContext:Platform::GetLiveContext());
    LiveRhiScope scope(Platform::LiveEventKind::CpuWork,Platform::LiveStage::Submit,"Validate + native submit");
    scope.event.outcome=Platform::LiveOutcome::Failed;
    if(!commands || !count) return false;
    for(uint32_t i=0;i<count;++i)if(commands[i]&&m_userCommands.count(commands[i]))
    {
        commands[i]->FlushLiveDeferred(commands[i]->m_liveDeferred->m_liveContext);
    }
    const uint64_t completed=GetCompletedSubmissionNative();
    bool needsPreparation=false;
    for(uint32_t i=0;i<count;++i)
    {
        if(!commands[i] || !m_userCommands.count(commands[i]) || !commands[i]->m_recordingClosed ||
            commands[i]->m_recordingFailed || commands[i]->m_completion>completed ||
            (commands[i]->m_imageGeneration && !m_frameActive) ||
            (commands[i]->m_imageGeneration && commands[i]->m_imageGeneration!=m_imageGeneration)) return false;
        for(uint32_t j=0;j<i;++j) if(commands[i]==commands[j]) return false;
        if(!commands[i]->m_preparedNative)needsPreparation=true;
    }
    // 제출이 성공할 때까지 전역 상태를 유지하고 배리어로 바뀐 상태만 따로 보관한다.
    ICommandList::StateMap states;
    // Texture requirements commonly visit consecutive mip levels. Reuse the
    // previous map node or its successor before searching the full state map.
    // This cursor lives only in this validation loop; barriers remain in states.
    auto lastOriginal=m_resourceStates.cend();
    for(uint32_t i=0;i<count;++i)for(const auto& operation:commands[i]->m_stateOperations)
    {
        const auto key=std::make_tuple(operation.resource,operation.mip,operation.layer);
        const auto changed=states.find(key);
        const ResourceState* current=nullptr;
        if(changed!=states.end()) current=&changed->second;
        else
        {
            auto original=lastOriginal;
            if(original==m_resourceStates.cend() || original->first!=key)
            {
                if(original!=m_resourceStates.cend())++original;
                if(original==m_resourceStates.cend() || original->first!=key)
                    original=m_resourceStates.find(key);
                lastOriginal=original;
            }
            if(original!=m_resourceStates.cend()) current=&original->second;
        }
        if(!current || *current!=operation.before)
        {
            ReportDiagnostic(DiagnosticSeverity::Error, "Submit: resource state mismatch. Barrier.before must match the current state (Undefined is not a wildcard); bound resources must still have the required state at draw/dispatch.");
            return false;
        }
        if(operation.barrier)
        {
            if(changed!=states.end()) changed->second=operation.after;
            else states.emplace(key,operation.after);
        }
    }
    std::vector<ICommandList*> native(count);
    if(needsPreparation && !PrepareCommandLists(commands, count)) return false;
    for(uint32_t i=0;i<count;++i)
    {
        native[i]=commands[i]->m_preparedNative;
        // SubmitNative takes ownership and may destroy a failed native list.
        commands[i]->m_preparedNative=nullptr;
    }
    const auto discard=[&] {for(auto* list:native) DiscardCommandListNative(list);};
    bool submitted=false;
    try { submitted=SubmitNative(native.data(),count); }
    catch(...) { discard(); throw; }
    if(!submitted)
    {
        discard();
        SubmitLiveWork(commands,count,false,0);
        ReportDiagnostic(DiagnosticSeverity::Error,"Submit: native submission failed.");
        return false;
    }
    for(const auto& entry:states) {
        const auto found=m_resourceStates.find(entry.first);
        if(found!=m_resourceStates.end())found->second=entry.second;
    }
    const uint64_t completion=GetLastSubmissionNative();
    for(uint32_t i=0;i<count;++i) commands[i]->m_completion=completion;
    SubmitLiveWork(commands,count,true,completion);
    scope.event.outcome=execution.outcome=Platform::LiveOutcome::Submitted;
    return true;
}

BufferHandle IDevice::CreateBuffer(const BufferDesc& desc)
{
    RequireOwnerThread();
    if(!desc.size || desc.usage == BufferUsage::None || !IsBufferStateAllowed(desc, desc.initialState)) return nullptr;
    auto* buffer = CreateBufferNative(desc);
    if(!buffer) ReportDiagnostic(DiagnosticSeverity::Error,"CreateBuffer: native size, usage or initial-state constraints (or allocation failure) prevented creation.");
    if(buffer) { ++m_allocationCounters.buffers.live; ++m_allocationCounters.buffers.created; }
    Track(buffer, [](IDevice& device, void* handle) {
        device.DestroyBufferNative(static_cast<BufferHandle>(handle));
        --device.m_allocationCounters.buffers.live; ++device.m_allocationCounters.buffers.destroyed;
    },{},Platform::LiveObjectKind::Buffer,0,desc.size);
    try { if(buffer)m_resourceStates[{reinterpret_cast<uintptr_t>(buffer),0,0}]=desc.initialState; }
    catch(...) { UnregisterResource(buffer); throw; }
    return buffer;
}

TextureHandle IDevice::CreateTexture(const TextureDesc& desc)
{
    RequireOwnerThread();
    if(!desc.width || !desc.height || !desc.depthOrArraySize || !desc.mipLevels
        || desc.width > GetLimit(Limit::Texture2DDimension) || desc.height > GetLimit(Limit::Texture2DDimension)
        || desc.depthOrArraySize > GetLimit(Limit::TextureArrayLayers)
        || desc.mipLevels > MaximumMipCount(desc.width, desc.height)
        || (!FormatSize(desc.format) && !IsBlockCompressed(desc.format)) || desc.usage == TextureUsage::None
        || (IsBlockCompressed(desc.format) && desc.usage != TextureUsage::ShaderResource)) return nullptr;
    auto* texture = CreateTextureNative(desc);
    if(!texture) ReportDiagnostic(DiagnosticSeverity::Error,"CreateTexture: native format, extent or usage constraints (or allocation failure) prevented creation; no format was substituted.");
    if(texture) { ++m_allocationCounters.textures.live; ++m_allocationCounters.textures.created; }
    Track(texture, [](IDevice& device, void* handle) {
        device.DestroyTextureNative(static_cast<TextureHandle>(handle));
        --device.m_allocationCounters.textures.live; ++device.m_allocationCounters.textures.destroyed;
    },{},Platform::LiveObjectKind::Texture);
    try
    {
        if(texture)for(uint32_t layer=0;layer<desc.depthOrArraySize;++layer)for(uint32_t mip=0;mip<desc.mipLevels;++mip)
            m_resourceStates[{reinterpret_cast<uintptr_t>(texture),mip,layer}]=ResourceState::Undefined;
    }
    catch(...) { UnregisterResource(texture); throw; }
    return texture;
}

ShaderHandle IDevice::CreateShader(const ShaderDesc& desc)
{
    RequireOwnerThread();
    const bool validStage = desc.stage == ShaderStage::Vertex || desc.stage == ShaderStage::Hull ||
        desc.stage == ShaderStage::Domain || desc.stage == ShaderStage::Fragment ||
        desc.stage == ShaderStage::Compute || desc.stage == ShaderStage::Mesh;
    if((desc.stage==ShaderStage::Compute && !Supports(Feature::Compute)) || !validStage ||
        (desc.stage==ShaderStage::Mesh && !Supports(Feature::MeshShader)) ||
        !desc.binary || !desc.binarySize || !desc.entryPoint || !*desc.entryPoint) return nullptr;
    auto* shader = CreateShaderNative(desc);
    if(!shader) ReportDiagnostic(DiagnosticSeverity::Error,"CreateShader: native shader creation failed; no substitute shader was selected.");
    Track(shader, [](IDevice& device, void* handle) { device.DestroyShaderNative(static_cast<ShaderHandle>(handle)); },
        {},Platform::LiveObjectKind::Shader,0,desc.binarySize,LiveShaderStage(desc.stage));
    return shader;
}


PipelineHandle IDevice::CreateComputePipeline(const ComputePipelineDesc& desc)
{
    RequireOwnerThread();
    if(!Supports(Feature::Compute))
    {ReportDiagnostic(DiagnosticSeverity::Error,"CreateComputePipeline: this backend/device does not support Compute.");return nullptr;}
    auto shader=Reference(desc.computeShader);
    if(!shader || desc.computeShader->GetStage()!=ShaderStage::Compute ||
        !Supports(desc.layout) ||
        (desc.layout.inlineConstantSize && desc.layout.inlineConstantStages!=ShaderStageFlags::Compute))return nullptr;
    for(uint32_t i=0;i<desc.layout.bindingCount;++i)
        if(desc.layout.bindings[i].stages!=ShaderStageFlags::Compute)return nullptr;
    std::vector<std::shared_ptr<void>> dependencies{shader};
    auto* pipeline=CreateComputePipelineNative(desc);
    if(!pipeline) ReportDiagnostic(DiagnosticSeverity::Error,"CreateComputePipeline: native creation failed; no substitute pipeline was selected.");
    if(pipeline) { ++m_allocationCounters.pipelines.live; ++m_allocationCounters.pipelines.created; }
    Track(pipeline,[](IDevice& device,void* handle){
        device.DestroyPipelineNative(static_cast<PipelineHandle>(handle));
        --device.m_allocationCounters.pipelines.live; ++device.m_allocationCounters.pipelines.destroyed;
    },std::move(dependencies),Platform::LiveObjectKind::Pipeline,0,0,"Compute pipeline; stages=Compute");
    return pipeline;
}

PipelineHandle IDevice::CreateGraphicsPipeline(const GraphicsPipelineDesc& desc)
{
    RequireOwnerThread();
    auto vertex = Reference(desc.vertexShader), hull = Reference(desc.hullShader);
    auto domain = Reference(desc.domainShader), fragment = Reference(desc.fragmentShader);
    if(!Supports(desc))
    {ReportDiagnostic(DiagnosticSeverity::Error,"CreateGraphicsPipeline: unsupported description or device limit. Query Supports(desc) before creation.");return nullptr;}
    // Complete fallible metadata allocation before acquiring the native object.
    // Track owns all failures once its shared_ptr construction begins.
    std::vector<std::shared_ptr<void>> dependencies{vertex, hull, domain, fragment};
    std::vector<VertexBufferLayout> vertexBuffers;
    std::vector<Format> colorFormats;
    if(desc.vertexBufferCount)vertexBuffers.assign(desc.vertexBuffers,desc.vertexBuffers+desc.vertexBufferCount);
    for(uint32_t i=0;i<desc.colorAttachmentCount;++i)
        colorFormats.push_back(desc.colorAttachments[i].format);
    auto* pipeline = CreateGraphicsPipelineNative(desc);
    if(!pipeline) ReportDiagnostic(DiagnosticSeverity::Error,"CreateGraphicsPipeline: native creation failed; no fallback pipeline was substituted.");
    if(pipeline)
    {
        pipeline->m_vertexBuffers = std::move(vertexBuffers);
        pipeline->m_colorFormats = std::move(colorFormats);
        pipeline->m_depthStencilFormat=desc.depthStencil.format;
        pipeline->m_topology=desc.topology;
        pipeline->m_usesStencil=desc.depthStencil.stencilEnabled;
        pipeline->m_requiresDepthWrite=desc.depthStencil.depthWriteEnabled||MayWriteStencil(desc.depthStencil);
    }
    if(pipeline) { ++m_allocationCounters.pipelines.live; ++m_allocationCounters.pipelines.created; }
    char description[192];std::snprintf(description,sizeof(description),"Graphics pipeline; topology=%s; stages=Vertex%s%s%s; patchControlPoints=%u",
        LiveTopology(desc.topology),desc.hullShader?",Hull":"",desc.domainShader?",Domain":"",desc.fragmentShader?",Fragment":"",desc.patchControlPoints);
    Track(pipeline, [](IDevice& device, void* handle) {
        device.DestroyPipelineNative(static_cast<PipelineHandle>(handle));
        --device.m_allocationCounters.pipelines.live; ++device.m_allocationCounters.pipelines.destroyed;
    }, std::move(dependencies),Platform::LiveObjectKind::Pipeline,0,0,description);
    return pipeline;
}

PipelineHandle IDevice::CreateMeshPipeline(const MeshPipelineDesc& desc)
{
    RequireOwnerThread();
    auto mesh = Reference(desc.meshShader), fragment = Reference(desc.fragmentShader);
    if(!mesh || (desc.fragmentShader && !fragment) || !Supports(desc))
    {ReportDiagnostic(DiagnosticSeverity::Error,"CreateMeshPipeline: unsupported description or device limit. Query Supports(desc) before creation.");return nullptr;}
    std::vector<std::shared_ptr<void>> dependencies{mesh, fragment};
    std::vector<Format> colorFormats;
    for(uint32_t i=0;i<desc.colorAttachmentCount;++i)
        colorFormats.push_back(desc.colorAttachments[i].format);
    auto* pipeline = CreateMeshPipelineNative(desc);
    if(!pipeline) ReportDiagnostic(DiagnosticSeverity::Error,"CreateMeshPipeline: native creation failed; no fallback pipeline was substituted.");
    if(pipeline)
    {
        pipeline->m_colorFormats = std::move(colorFormats);
        pipeline->m_depthStencilFormat=desc.depthStencil.format;
        pipeline->m_usesStencil=desc.depthStencil.stencilEnabled;
        pipeline->m_requiresDepthWrite=desc.depthStencil.depthWriteEnabled||MayWriteStencil(desc.depthStencil);
    }
    if(pipeline) { ++m_allocationCounters.pipelines.live; ++m_allocationCounters.pipelines.created; }
    Track(pipeline, [](IDevice& device, void* handle) {
        device.DestroyPipelineNative(static_cast<PipelineHandle>(handle));
        --device.m_allocationCounters.pipelines.live; ++device.m_allocationCounters.pipelines.destroyed;
    }, std::move(dependencies),Platform::LiveObjectKind::Pipeline,0,0,desc.fragmentShader?"Mesh pipeline; stages=Mesh,Fragment":"Mesh pipeline; stages=Mesh");
    return pipeline;
}

ResourceSetHandle IDevice::CreateResourceSet(const ResourceSetDesc& desc)
{
    RequireOwnerThread();
    std::vector<std::shared_ptr<void>> dependencies;
    auto pipeline = Reference(desc.pipeline);
    if(!pipeline || (desc.bindingCount && !desc.bindings)) return nullptr;
    dependencies.push_back(std::move(pipeline));
    for(uint32_t i = 0; i < desc.bindingCount; ++i)
    {
        const auto& binding = desc.bindings[i];
        if(binding.buffer && binding.size==0)
        {
            ReportDiagnostic(DiagnosticSeverity::Error,
                "CreateResourceSet: buffer bindings require an explicit nonzero byte range; size=0 does not select the whole buffer.");
            return nullptr;
        }
        const void* handles[] = {binding.buffer, binding.texture};
        for(const void* handle : handles)
            if(handle)
            {
                auto reference = Reference(handle);
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
        const auto* found = FindLayoutBinding(layout, binding.binding);
        if(!found) return nullptr;
        const bool uniform = found->type == ResourceBindingType::ConstantBuffer;
        const auto alignment = GetLimit(uniform ? Limit::UniformBufferOffsetAlignment : Limit::StorageBufferOffsetAlignment);
        const auto maximum = GetLimit(uniform ? Limit::UniformBufferBytes : Limit::StorageBufferBytes);
        if(!alignment || binding.offset % alignment || binding.size > maximum)
        {ReportDiagnostic(DiagnosticSeverity::Error,"CreateResourceSet: buffer binding exceeds device range or offset alignment. Query GetLimit for the required values.");return nullptr;}
    }
    auto* resources = CreateResourceSetNative(desc);
    if(!resources) ReportDiagnostic(DiagnosticSeverity::Error,"CreateResourceSet: native binding constraints or allocation prevented creation; no substitute resource was bound.");
    Track(resources, [](IDevice& device, void* handle) { device.DestroyResourceSetNative(static_cast<ResourceSetHandle>(handle)); },
        std::move(dependencies),Platform::LiveObjectKind::ResourceSet,desc.bindingCount);
    return resources;
}

void IDevice::DestroyBuffer(BufferHandle handle) { RequireOwnerThread(); RecordLiveRelease(handle);UnregisterResource(handle); }
void IDevice::DestroyTexture(TextureHandle handle)
{
    RequireOwnerThread();
    if(!m_borrowedTextures.count(handle)) {RecordLiveRelease(handle);UnregisterResource(handle);}
}
void IDevice::DestroyShader(ShaderHandle handle) { RequireOwnerThread(); RecordLiveRelease(handle);UnregisterResource(handle); }
void IDevice::DestroyPipeline(PipelineHandle handle) { RequireOwnerThread(); RecordLiveRelease(handle);UnregisterResource(handle); }
void IDevice::DestroyResourceSet(ResourceSetHandle handle) { RequireOwnerThread(); RecordLiveRelease(handle);UnregisterResource(handle); }

bool IDevice::UpdateBuffer(ICommandList& commands, BufferHandle buffer, uint32_t offset, const void* data, uint32_t size)
try
{
    if(commands.m_owner != this || !commands.Track(buffer) || !data || !size
        || offset > buffer->GetDesc().size || size > buffer->GetDesc().size - offset)
    { commands.m_recordingFailed = true; return false; }
    const size_t position=commands.m_commandBytes.size();
    const auto* first=static_cast<const uint8_t*>(data);
    // Keep the required input copy in the list's reusable payload storage. Store
    // an offset: subsequent commands may grow and relocate the byte vector.
    // Large asset uploads retain separate blocks rather than forcing one giant
    // allocation and copying earlier uploads every time that allocation grows.
    std::vector<uint8_t> bytes;
    const uint64_t storedSize=(uint64_t(size)+3u)&~uint64_t(3u);
    if(position<=RecordingState::PayloadLimit && storedSize<=RecordingState::PayloadLimit-position)
    {
        commands.m_commandBytes.insert(commands.m_commandBytes.end(),first,first+size);
        commands.m_commandBytes.resize(position+storedSize,0); // Preserve inline-constant alignment.
    }
    else bytes.assign(first,first+size);
    commands.RequireState(buffer,0,0,ResourceState::CopyDestination);
    commands.m_commands.push_back([this,buffer,offset,size,position,source=&commands,bytes=std::move(bytes)](ICommandList& native) {
        const auto* data=bytes.empty()?source->m_commandBytes.data()+position:bytes.data();
        if(UpdateBufferNative(native,buffer,offset,data,size))return true;
        std::fprintf(stderr, "[RHI] dyf::RHI [error]: Submit: UpdateBuffer failed (offset=%u, size=%u, bufferSize=%u).\n",
            offset,size,buffer->GetDesc().size);
        return false;
    });
    commands.RecordLiveCommand(Platform::LiveCommandKind::Upload,{offset,size},buffer,0,size,1,
        Platform::LiveEventKind::Upload,offset==0&&size==buffer->GetDesc().size);
    return true;
}
catch(...) { commands.m_recordingFailed=true; throw; }

bool IDevice::UpdateTexture(ICommandList& commands, TextureHandle texture, uint32_t mip, uint32_t layer,
    const void* data, uint32_t size, uint32_t rowPitch, uint32_t slicePitch)
try
{
    if(commands.m_owner != this || !commands.Track(texture) || !data || !size
        || mip >= texture->GetDesc().mipLevels || layer >= texture->GetDesc().depthOrArraySize)
    { commands.m_recordingFailed = true; return false; }
    const auto format=texture->GetDesc().format;
    const bool compressed=IsBlockCompressed(format);
    const uint64_t width=std::max(1u, texture->GetDesc().width >> mip);
    const uint64_t texelHeight=std::max(1u, texture->GetDesc().height >> mip);
    const uint64_t row=compressed ? ((width+3)/4)*CompressedBlockBytes(format) : width*FormatSize(format);
    const uint64_t height=compressed ? (texelHeight+3)/4 : texelHeight;
    if(rowPitch < row || slicePitch < uint64_t(rowPitch) * height || size < uint64_t(rowPitch) * (height - 1) + row)
    {
        ReportDiagnostic(DiagnosticSeverity::Error,"UpdateTexture: rowPitch must cover one row, slicePitch must cover rowPitch*height, and dataSize must cover the last row's pixels. Trailing row padding is not required.");
        commands.m_recordingFailed = true; return false;
    }
    const size_t position=commands.m_commandBytes.size();
    const auto* first=static_cast<const uint8_t*>(data);
    std::vector<uint8_t> bytes;
    const uint64_t storedSize=(uint64_t(size)+3u)&~uint64_t(3u);
    if(position<=RecordingState::PayloadLimit && storedSize<=RecordingState::PayloadLimit-position)
    {
        commands.m_commandBytes.insert(commands.m_commandBytes.end(),first,first+size);
        commands.m_commandBytes.resize(position+storedSize,0);
    }
    else bytes.assign(first,first+size);
    commands.RequireState(texture,mip,layer,ResourceState::CopyDestination);
    commands.m_commands.push_back([this,texture,mip,layer,rowPitch,slicePitch,size,position,source=&commands,bytes=std::move(bytes)](ICommandList& native) {
        const auto* data=bytes.empty()?source->m_commandBytes.data()+position:bytes.data();
        if(UpdateTextureNative(native,texture,mip,layer,data,size,rowPitch,slicePitch))return true;
        std::fprintf(stderr, "[RHI] dyf::RHI [error]: Submit: UpdateTexture failed (mip=%u, layer=%u, size=%u, rowPitch=%u, slicePitch=%u).\n",
            mip,layer,size,rowPitch,slicePitch);
        return false;
    });
    commands.RecordLiveCommand(Platform::LiveCommandKind::Upload,{mip,layer,size,rowPitch,slicePitch},texture,0,size,1,
        Platform::LiveEventKind::Upload,texture->GetDesc().mipLevels==1&&texture->GetDesc().depthOrArraySize==1);
    return true;
}
catch(...) { commands.m_recordingFailed=true; throw; }

bool IDevice::ReadTexture(TextureHandle texture, TextureReadback& result)
{
    RequireOwnerThread();
    auto* live=GetLiveState();
    Platform::LiveFrameScope frame(live?live->device:0,live?live->device:0);
    LiveRhiScope scope(Platform::LiveEventKind::CpuWork,Platform::LiveStage::Output,"Read texture");
    frame.SetOutcome(Platform::LiveOutcome::Failed);
    scope.event.resource=Platform::FindLiveObject(texture);scope.event.operations=1;
    scope.event.outcome=Platform::LiveOutcome::Failed;
    if(!Reference(texture)){frame.SetOutcome(scope.event.outcome);return false;}
    const bool ready=ReadTextureNative(texture,result);
    scope.event.outcome=ready?Platform::LiveOutcome::Completed:Platform::LiveOutcome::Unknown;
    if(ready)scope.event.bytes=result.pixels.size();
    frame.SetOutcome(scope.event.outcome);return ready;
}
}
