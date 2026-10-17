#include "dyf/Platform/Log.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/ICommandList.h"
#include "RHI/Validation.h"
#include "dyf/RHI/Readback.h"
#include "dyf/Core/ThreadPool.h"
#include "LiveRhi.h"
#include <chrono>
#include <thread>
#include <cstdio>

namespace dyf::RHI
{
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
IDevice::IDevice() = default;
IDevice::~IDevice()
{
#ifdef DY_LIVE_PROFILING_ENABLED
    if(m_live)
    {
        m_live->stopping=true;m_live->wake.notify_all();
        if(m_live->worker.joinable())m_live->worker.join();
        Diagnostics::RetireLiveObject(m_live->recordingSource);
        Diagnostics::RetireLiveObject(m_live->device);
    }
#endif
}

void IDevice::ReportDiagnostic(DiagnosticSeverity severity, const char* message) const
{
    const auto level = severity == DiagnosticSeverity::Error ? Platform::LogLevel::Error :
        severity == DiagnosticSeverity::Warning ? Platform::LogLevel::Warning : Platform::LogLevel::Info;
    Platform::Log::Write(level, "RHI", message ? message : "");
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
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
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
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    if(!desc.count || !Supports(Feature::TimestampQuery)) return nullptr;
    auto* query=CreateTimestampQueryNative(desc);
    Track(query,[](IDevice& device,void* object){device.DestroyTimestampQueryNative(static_cast<TimestampQueryHandle>(object));},
        {},Diagnostics::LiveObjectKind::Query,desc.count,uint64_t(desc.count)*sizeof(uint64_t));
    return query;
}
void IDevice::DestroyTimestampQuery(TimestampQueryHandle query)
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    RecordLiveRelease(query);
    m_resources.erase(query);
}
bool IDevice::ReadTimestamps(TimestampQueryHandle query,uint32_t first,uint32_t count,uint64_t* ticks)
{return ReadTimestampsInternal(query,first,count,ticks,true);}

bool IDevice::ReadTimestampsInternal(TimestampQueryHandle query,uint32_t first,uint32_t count,uint64_t* ticks,bool observe)
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    auto* live=observe?GetLiveState():nullptr;
    Diagnostics::LiveFrameScope frame(live?live->device:0,live?live->device:0);
    frame.SetOutcome(Diagnostics::LiveOutcome::Failed);
    LiveRhiScope scope(Diagnostics::LiveEventKind::CpuWork,Diagnostics::LiveStage::Output,"Read timestamps",observe);
    scope.event.resource=Diagnostics::FindLiveObject(query);scope.event.arguments={first,count};scope.event.operations=1;
    scope.event.outcome=Diagnostics::LiveOutcome::Failed;
    if(!Reference(query) || !ticks || !count || first>=query->GetCount() || count>query->GetCount()-first)
    {frame.SetOutcome(Diagnostics::LiveOutcome::Failed);return false;}
    const bool ready=ReadTimestampsNative(query,first,count,ticks);
    scope.event.outcome=ready?Diagnostics::LiveOutcome::Completed:Diagnostics::LiveOutcome::Unknown;
    if(ready)scope.event.bytes=uint64_t(count)*sizeof(uint64_t);
    frame.SetOutcome(scope.event.outcome);return ready;
}
bool IDevice::GetTimestampCalibration(TimestampCalibration& calibration) const
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
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
    auto* live=GetLiveState();
    Diagnostics::LiveFrameScope frame(live?live->device:0,live?live->device:0);
    LiveRhiScope scope(Diagnostics::LiveEventKind::Wait,Diagnostics::LiveStage::Submit,"WaitIdle");
    frame.SetOutcome(Diagnostics::LiveOutcome::Failed);scope.event.outcome=Diagnostics::LiveOutcome::Failed;scope.event.operations=1;
    const bool complete=WaitIdleNative();
    scope.event.operations=1;scope.event.outcome=complete?Diagnostics::LiveOutcome::Completed:Diagnostics::LiveOutcome::Failed;
    frame.SetOutcome(scope.event.outcome);
    return complete;
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
    auto* live=GetLiveState();
    if(live)
    {live->frame=Diagnostics::BeginLiveFrame(live->device,live->device);live->hasFrame=live->frame.key.frame!=0;PrepareLiveControls(live->frame.key);}
    LiveRhiScope scope(Diagnostics::LiveEventKind::Output,Diagnostics::LiveStage::Output,"BeginFrame / acquire");
    scope.event.operations=1;
    InvalidateBackBuffers();
    m_frameActive = BeginFrameNative();
    if(live)
    {
        scope.event.outcome=m_frameActive?Diagnostics::LiveOutcome::Observed:
            (m_hasSwapchain&&!IsLostNative()?Diagnostics::LiveOutcome::Skipped:Diagnostics::LiveOutcome::Failed);
        if(scope.event.outcome==Diagnostics::LiveOutcome::Skipped)
        {scope.event.kind=Diagnostics::LiveEventKind::Wait;scope.event.id=scope.event.context.frame.frame;}
        scope.Finish();
        if(!m_frameActive&&live->hasFrame)
        {Diagnostics::EndLiveFrame(live->frame,scope.event.outcome);live->hasFrame=false;}
    }
    return m_frameActive;
}

bool IDevice::Present()
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    if(!m_frameActive) {dyf::Platform::Log::Writef(dyf::Platform::LogLevel::Error, "RHI", __FILE__, __LINE__, "dyf::RHI: Present requires an active frame.");return false;}
    auto* backBuffer = GetBackBuffer();
    const auto state = m_resourceStates.find({reinterpret_cast<uintptr_t>(backBuffer),0,0});
    if(!backBuffer || state == m_resourceStates.end() || state->second != ResourceState::Present)
    {dyf::Platform::Log::Writef(dyf::Platform::LogLevel::Error, "RHI", __FILE__, __LINE__, "dyf::RHI: Submit the backbuffer transition to Present before presenting.");return false;}
    LiveRhiScope scope(Diagnostics::LiveEventKind::Output,Diagnostics::LiveStage::Output,"Present");
    scope.event.commandKind=Diagnostics::LiveCommandKind::Present;
    scope.event.resource=Diagnostics::FindLiveObject(backBuffer);scope.event.operations=1;
    if(m_live)scope.event.submission=m_live->backBufferSubmission;
    const bool presented = PresentNative();
    m_frameActive = false;
    scope.event.outcome=presented&&!IsLostNative()?Diagnostics::LiveOutcome::Completed:Diagnostics::LiveOutcome::Failed;
    scope.Finish();
    if(m_live&&m_live->hasFrame)
    {Diagnostics::EndLiveFrame(m_live->frame,scope.event.outcome);m_live->hasFrame=false;}
    if(!presented || IsLostNative()) {dyf::Platform::Log::Writef(dyf::Platform::LogLevel::Error, "RHI", __FILE__, __LINE__, "dyf::RHI: Presentation failed.");return false;}
    return true;
}


std::shared_ptr<void> IDevice::Reference(const void* handle) const
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    const auto found = m_resources.find(handle);
    return found == m_resources.end() ? nullptr : found->second;
}

void IDevice::Track(void* handle, void (*destroy)(IDevice&, void*),
    std::vector<std::shared_ptr<void>> dependencies,Diagnostics::LiveObjectKind kind,uint64_t elements,uint64_t bytes,const char* label)
{
    if(!handle) return;
    Diagnostics::LiveId liveId=0;
    if(auto* live=GetLiveState())liveId=Diagnostics::RegisterLiveObject(handle,
        {kind,live->device,Diagnostics::GetLiveContext().owner,elements,bytes,label});
    auto reference = std::shared_ptr<void>(handle,
        [this, destroy, liveId, dependencies = std::move(dependencies)](void* object)
        {
            std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
            const uintptr_t key=reinterpret_cast<uintptr_t>(object);
            for(auto it=m_resourceStates.begin();it!=m_resourceStates.end();)
                if(std::get<0>(it->first)==key)it=m_resourceStates.erase(it);else ++it;
            if(m_nativeResourcesAlive) {destroy(*this, object);Diagnostics::RetireLiveObject(liveId);}
            else
            {
                Diagnostics::LiveEvent event;event.context=Diagnostics::GetLiveContext();event.resource=liveId;
                event.kind=Diagnostics::LiveEventKind::CpuWork;event.stage=Diagnostics::LiveStage::Data;
                event.outcome=Diagnostics::LiveOutcome::Unknown;Diagnostics::RecordLiveEvent(event);
            }
            PublishLiveResources();
        });
    m_resources.emplace(handle, std::move(reference));
    PublishLiveResources();
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
    StopLiveProfiling();
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
    const auto rollback = [this](ICommandList* value) {
        m_userCommands.erase(value);
        delete value;
    };
    std::unique_ptr<ICommandList, decltype(rollback)> owner(ICommandList::CreateRecorded(), rollback);
    auto* commands = owner.get();
    commands->m_owner = this;
    if(auto* live=GetLiveState())
    {
        commands->m_livePosition={Diagnostics::RegisterLiveObject(commands,
            {Diagnostics::LiveObjectKind::CommandList,live->device,Diagnostics::GetLiveContext().owner,0,0,"Command list"}),1,0};
    }
    m_userCommands.insert(commands);
    m_recordedCommands.insert(commands);
    BeginLiveRecording(*commands);
    return owner.release();
}

TextureHandle IDevice::GetBackBuffer()
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    auto* texture = GetBackBufferNative();
    if(texture && !Reference(texture))
    {
        m_borrowedTextures.insert(texture);
        m_resources.emplace(texture, std::shared_ptr<void>(texture, [](void*) {}));
        if(auto* live=GetLiveState())Diagnostics::RegisterLiveObject(texture,
            {Diagnostics::LiveObjectKind::Texture,live->device,live->device,0,0,"Swapchain image (borrowed)"});
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
    auto* live=GetLiveState();
    Diagnostics::LiveFrameScope frame(live?live->device:0,live?live->device:0);
    LiveRhiScope scope(Diagnostics::LiveEventKind::Wait,Diagnostics::LiveStage::Submit,"Wait fence");
    frame.SetOutcome(Diagnostics::LiveOutcome::Failed);
    scope.event.operations=1;scope.event.submission=GetLiveSubmissionId(completion);
    scope.event.outcome=Diagnostics::LiveOutcome::Failed;
    if(completion.m_device!=this || !completion.m_value) return false;
    const auto start=std::chrono::steady_clock::now();
    do
    {
        if(IsComplete(completion)) {scope.event.outcome=Diagnostics::LiveOutcome::Completed;frame.SetOutcome(scope.event.outcome);return true;}
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
    commands->FinishLiveRecording(commands->m_recordingFailed?Diagnostics::LiveOutcome::Failed:Diagnostics::LiveOutcome::Discarded,true);
    if(commands->m_preparedNative)
    {
        DiscardCommandListNative(commands->m_preparedNative);
        commands->m_preparedNative = nullptr;
    }
    DiscardLivePrepared(commands,Diagnostics::LiveOutcome::Discarded);
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
    if(commands->m_livePosition.commandList)++commands->m_livePosition.generation;
    commands->m_livePosition.index=0;
    commands->m_liveContext={};commands->m_liveExecutionContext={};commands->m_liveRecordFrame={};
    commands->m_liveInExecution=commands->m_liveSubmitted=commands->m_liveTerminal=false;
    commands->m_liveCommandBeginNs=0;commands->m_livePreviousEvent=0;commands->DiscardLiveDeferred();
    BeginLiveRecording(*commands);
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
        for(const auto& item : work)
        {DiscardCommandListNative(item.native);DiscardLivePrepared(item.source,Diagnostics::LiveOutcome::Failed);}
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
                    commands[i]->m_completion > completed ||
                    (commands[i]->m_imageGeneration && !m_frameActive) ||
                    (commands[i]->m_imageGeneration && commands[i]->m_imageGeneration != m_imageGeneration)) return false;
                for(uint32_t j = 0; j < i; ++j) if(commands[i] == commands[j]) return false;
            }
            // Native pools/allocators are allocated under the device lock. Each list
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
                const auto slot=ReserveLiveQuery(commands[i]);
                if(slot!=UINT32_MAX&&m_live)
                {
                    try
                    {
                        native->m_references.push_back(Reference(m_live->query));
                        native->m_liveReplayQuery=m_live->query;native->m_liveFirstQuery=slot*2;
                    }
                    catch(...){DiscardLivePrepared(commands[i],Diagnostics::LiveOutcome::Skipped);}
                }
            }
        }
        if(work.empty())return true;
        LiveExecutionScope execution(*this,commands,count);
        std::vector<uint8_t> recorded(work.size(), 0);
        const auto callerContext=Diagnostics::GetLiveContext();
        const auto record = [&](size_t i) {
            Diagnostics::LiveContextScope context(work[i].source->m_liveExecutionContext.frame.frame?
                work[i].source->m_liveExecutionContext:callerContext);
            LiveRhiScope scope(Diagnostics::LiveEventKind::NativeTranslation,Diagnostics::LiveStage::NativeTranslate,"Native replay");
            scope.event.command=work[i].source->m_livePosition;scope.event.operations=1;
            scope.event.outcome=Diagnostics::LiveOutcome::Failed;
            recorded[i] = work[i].native->ReplayNative(work[i].source->m_commands);
            scope.event.outcome=recorded[i]?Diagnostics::LiveOutcome::Observed:Diagnostics::LiveOutcome::Failed;
            Diagnostics::LiveEvent policy;policy.context=Diagnostics::GetLiveContext();policy.command=work[i].source->m_livePosition;
            policy.kind=Diagnostics::LiveEventKind::CpuWork;policy.stage=Diagnostics::LiveStage::NativeTranslate;
            policy.arguments={work[i].source->m_uploadPageReuse,work[i].source->m_uploadSuballocation};
            const char label[]="Captured upload policy: reuse, suballocation";std::copy(std::begin(label),std::end(label),policy.label);
            if(policy.context.frame.frame)Diagnostics::RecordLiveEvent(policy);
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
        execution.outcome=Diagnostics::LiveOutcome::Observed;
        return true;
    }
    catch(...) { discard(); throw; }
}

bool IDevice::Submit(ICommandList** commands, uint32_t count)
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    LiveExecutionScope execution(*this,commands,count);
    Diagnostics::LiveContextScope context(commands&&count&&commands[0]&&m_userCommands.count(commands[0])?
        commands[0]->m_liveExecutionContext:Diagnostics::GetLiveContext());
    LiveRhiScope scope(Diagnostics::LiveEventKind::CpuWork,Diagnostics::LiveStage::Submit,"Validate + native submit");
    scope.event.outcome=Diagnostics::LiveOutcome::Failed;
    if(!commands || !count) return false;
    for(uint32_t i=0;i<count;++i)if(commands[i]&&m_userCommands.count(commands[i]))
    {
        commands[i]->FlushLiveDeferred(commands[i]->m_liveContext);
    }
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
    scope.event.outcome=execution.outcome=Diagnostics::LiveOutcome::Submitted;
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
    },{},Diagnostics::LiveObjectKind::Buffer,0,desc.size);
    try { if(buffer)m_resourceStates[{reinterpret_cast<uintptr_t>(buffer),0,0}]=desc.initialState; }
    catch(...) { m_resources.erase(buffer); throw; }
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
    },{},Diagnostics::LiveObjectKind::Texture);
    try
    {
        if(texture)for(uint32_t layer=0;layer<desc.depthOrArraySize;++layer)for(uint32_t mip=0;mip<desc.mipLevels;++mip)
            m_resourceStates[{reinterpret_cast<uintptr_t>(texture),mip,layer}]=ResourceState::Undefined;
    }
    catch(...) { m_resources.erase(texture); throw; }
    return texture;
}

ShaderHandle IDevice::CreateShader(const ShaderDesc& desc)
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    const bool validStage = desc.stage == ShaderStage::Vertex || desc.stage == ShaderStage::Hull ||
        desc.stage == ShaderStage::Domain || desc.stage == ShaderStage::Fragment ||
        desc.stage == ShaderStage::Compute || desc.stage == ShaderStage::Mesh;
    if((desc.stage==ShaderStage::Compute && !Supports(Feature::Compute)) || !validStage ||
        (desc.stage==ShaderStage::Mesh && !Supports(Feature::MeshShader)) ||
        !desc.binary || !desc.binarySize || !desc.entryPoint || !*desc.entryPoint) return nullptr;
    auto* shader = CreateShaderNative(desc);
    if(!shader) ReportDiagnostic(DiagnosticSeverity::Error,"CreateShader: native shader creation failed; no substitute shader was selected.");
    Track(shader, [](IDevice& device, void* handle) { device.DestroyShaderNative(static_cast<ShaderHandle>(handle)); },
        {},Diagnostics::LiveObjectKind::Shader,0,desc.binarySize,LiveShaderStage(desc.stage));
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
    std::vector<std::shared_ptr<void>> dependencies{shader};
    auto* pipeline=CreateComputePipelineNative(desc);
    if(!pipeline) ReportDiagnostic(DiagnosticSeverity::Error,"CreateComputePipeline: native creation failed; no substitute pipeline was selected.");
    if(pipeline) { ++m_allocationCounters.pipelines.live; ++m_allocationCounters.pipelines.created; }
    Track(pipeline,[](IDevice& device,void* handle){
        device.DestroyPipelineNative(static_cast<PipelineHandle>(handle));
        --device.m_allocationCounters.pipelines.live; ++device.m_allocationCounters.pipelines.destroyed;
    },std::move(dependencies),Diagnostics::LiveObjectKind::Pipeline,0,0,"Compute pipeline; stages=Compute");
    return pipeline;
}

PipelineHandle IDevice::CreateGraphicsPipeline(const GraphicsPipelineDesc& desc)
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    auto vertex = Reference(desc.vertexShader), hull = Reference(desc.hullShader);
    auto domain = Reference(desc.domainShader), fragment = Reference(desc.fragmentShader);
    if(!Supports(desc))
    {ReportDiagnostic(DiagnosticSeverity::Error,"CreateGraphicsPipeline: unsupported description or device limit. Query Supports(desc) before creation.");return nullptr;}
    // Complete fallible metadata allocation before acquiring the native object.
    // Track owns all failures once its shared_ptr construction begins.
    std::vector<std::shared_ptr<void>> dependencies{vertex, hull, domain, fragment};
    std::vector<uint32_t> requiredVertexBindings;
    std::vector<Format> colorFormats;
    for(uint32_t i=0;i<desc.vertexAttributeCount;++i)
    {
        const uint32_t binding=desc.vertexAttributes[i].binding;
        if(std::find(requiredVertexBindings.begin(),requiredVertexBindings.end(),binding)==requiredVertexBindings.end())
            requiredVertexBindings.push_back(binding);
    }
    for(uint32_t i=0;i<desc.colorAttachmentCount;++i)
        colorFormats.push_back(desc.colorAttachments[i].format);
    auto* pipeline = CreateGraphicsPipelineNative(desc);
    if(!pipeline) ReportDiagnostic(DiagnosticSeverity::Error,"CreateGraphicsPipeline: native creation failed; no fallback pipeline was substituted.");
    if(pipeline)
    {
        pipeline->m_requiredVertexBindings = std::move(requiredVertexBindings);
        pipeline->m_colorFormats = std::move(colorFormats);
        pipeline->m_depthStencilFormat=desc.depthStencil.format;
    }
    if(pipeline) { ++m_allocationCounters.pipelines.live; ++m_allocationCounters.pipelines.created; }
    char description[192];std::snprintf(description,sizeof(description),"Graphics pipeline; topology=%s; stages=Vertex%s%s%s; patchControlPoints=%u",
        LiveTopology(desc.topology),desc.hullShader?",Hull":"",desc.domainShader?",Domain":"",desc.fragmentShader?",Fragment":"",desc.patchControlPoints);
    Track(pipeline, [](IDevice& device, void* handle) {
        device.DestroyPipelineNative(static_cast<PipelineHandle>(handle));
        --device.m_allocationCounters.pipelines.live; ++device.m_allocationCounters.pipelines.destroyed;
    }, std::move(dependencies),Diagnostics::LiveObjectKind::Pipeline,0,0,description);
    return pipeline;
}

PipelineHandle IDevice::CreateMeshPipeline(const MeshPipelineDesc& desc)
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
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
    }
    if(pipeline) { ++m_allocationCounters.pipelines.live; ++m_allocationCounters.pipelines.created; }
    Track(pipeline, [](IDevice& device, void* handle) {
        device.DestroyPipelineNative(static_cast<PipelineHandle>(handle));
        --device.m_allocationCounters.pipelines.live; ++device.m_allocationCounters.pipelines.destroyed;
    }, std::move(dependencies),Diagnostics::LiveObjectKind::Pipeline,0,0,desc.fragmentShader?"Mesh pipeline; stages=Mesh,Fragment":"Mesh pipeline; stages=Mesh");
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
        std::move(dependencies),Diagnostics::LiveObjectKind::ResourceSet,desc.bindingCount);
    return resources;
}

void IDevice::DestroyBuffer(BufferHandle handle) { std::lock_guard<std::recursive_mutex> lock(m_resourceMutex); RecordLiveRelease(handle);m_resources.erase(handle); }
void IDevice::DestroyTexture(TextureHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    if(!m_borrowedTextures.count(handle)) {RecordLiveRelease(handle);m_resources.erase(handle);}
}
void IDevice::DestroyShader(ShaderHandle handle) { std::lock_guard<std::recursive_mutex> lock(m_resourceMutex); RecordLiveRelease(handle);m_resources.erase(handle); }
void IDevice::DestroyPipeline(PipelineHandle handle) { std::lock_guard<std::recursive_mutex> lock(m_resourceMutex); RecordLiveRelease(handle);m_resources.erase(handle); }
void IDevice::DestroyResourceSet(ResourceSetHandle handle) { std::lock_guard<std::recursive_mutex> lock(m_resourceMutex); RecordLiveRelease(handle);m_resources.erase(handle); }

bool IDevice::UpdateBuffer(ICommandList& commands, BufferHandle buffer, uint32_t offset, const void* data, uint32_t size)
try
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
        dyf::Platform::Log::Writef(dyf::Platform::LogLevel::Error, "RHI", __FILE__, __LINE__, "dyf::RHI [error]: Submit: UpdateBuffer failed (offset=%u, size=%zu, bufferSize=%u).",
            offset,bytes.size(),buffer->GetDesc().size);
        return false;
    });
    commands.RecordLiveCommand(Diagnostics::LiveCommandKind::Upload,{offset,size},buffer,0,size,1,
        Diagnostics::LiveEventKind::Upload,offset==0&&size==buffer->GetDesc().size);
    return true;
}
catch(...) { commands.m_recordingFailed=true; throw; }

bool IDevice::UpdateTexture(ICommandList& commands, TextureHandle texture, uint32_t mip, uint32_t layer,
    const void* data, uint32_t size, uint32_t rowPitch, uint32_t slicePitch)
try
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
        dyf::Platform::Log::Writef(dyf::Platform::LogLevel::Error, "RHI", __FILE__, __LINE__, "dyf::RHI [error]: Submit: UpdateTexture failed (mip=%u, layer=%u, size=%zu, rowPitch=%u, slicePitch=%u).",
            mip,layer,bytes.size(),rowPitch,slicePitch);
        return false;
    });
    commands.RecordLiveCommand(Diagnostics::LiveCommandKind::Upload,{mip,layer,size,rowPitch,slicePitch},texture,0,size,1,
        Diagnostics::LiveEventKind::Upload,texture->GetDesc().mipLevels==1&&texture->GetDesc().depthOrArraySize==1);
    return true;
}
catch(...) { commands.m_recordingFailed=true; throw; }

bool IDevice::ReadTexture(TextureHandle texture, TextureReadback& result)
{
    std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
    auto* live=GetLiveState();
    Diagnostics::LiveFrameScope frame(live?live->device:0,live?live->device:0);
    LiveRhiScope scope(Diagnostics::LiveEventKind::CpuWork,Diagnostics::LiveStage::Output,"Read texture");
    frame.SetOutcome(Diagnostics::LiveOutcome::Failed);
    scope.event.resource=Diagnostics::FindLiveObject(texture);scope.event.operations=1;
    scope.event.outcome=Diagnostics::LiveOutcome::Failed;
    if(!Reference(texture)){frame.SetOutcome(scope.event.outcome);return false;}
    const bool ready=ReadTextureNative(texture,result);
    scope.event.outcome=ready?Diagnostics::LiveOutcome::Completed:Diagnostics::LiveOutcome::Unknown;
    if(ready)scope.event.bytes=result.pixels.size();
    frame.SetOutcome(scope.event.outcome);return ready;
}
}
