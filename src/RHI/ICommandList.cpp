#include "dyf/RHI/ICommandList.h"
#include "Query.h"
#include "dyf/RHI/IDevice.h"
#include "Validation.h"
#include "../Platform/Profiler/RhiProfiler.h"
#include "../Platform/Profiler/CollectorInternal.h"
#include <algorithm>
#include <string>

namespace dyf::RHI
{
namespace D=Platform;
ICommandList::ICommandList() : m_liveDeferred(std::make_unique<LiveDeferred>()) {}
Platform::LiveCommandRef ICommandList::GetLiveCommandPosition() const noexcept
{
    return m_liveDeferred->m_livePosition;
}

namespace
{
#ifdef DY_LIVE_PROFILING_ENABLED
constexpr size_t LiveEventBatchSize=128;
constexpr size_t LiveLinkBatchSize=512;

template<class T> void DeferDetail(std::vector<T>& values,const T& value,size_t limit,uint64_t& omitted) noexcept
{
    if(values.size()>=limit){++omitted;return;}
    if(values.size()==values.capacity())
    {
        const auto oldBytes=values.capacity()*sizeof(T);
        const auto capacity=(std::min)(limit,(std::max)(size_t{16},values.capacity()*2));
        const auto extra=capacity*sizeof(T)-oldBytes;
        if(!D::ReserveLiveDetailBytes(extra)){++omitted;return;}
        try{values.reserve(capacity);}
        catch(...){D::ReleaseLiveDetailBytes(extra);++omitted;return;}
        const auto allocated=values.capacity()*sizeof(T)-oldBytes;
        if(allocated>extra&&!D::ReserveLiveDetailBytes(allocated-extra))
        {
            omitted+=values.size()+1;std::vector<T>().swap(values);
            D::ReleaseLiveDetailBytes(oldBytes+extra);return;
        }
    }
    values.push_back(value);
}
#endif
}
void ICommandList::DeferLiveEvent(const D::LiveEvent& event) noexcept
{
#ifdef DY_LIVE_PROFILING_ENABLED
    auto& deferred=*m_liveDeferred;
    if(deferred.m_liveContext.frame.frame &&
        (deferred.events.size() >= LiveEventBatchSize || deferred.links.size() >= LiveLinkBatchSize))
        FlushLiveDeferred(deferred.m_liveContext);
    D::AccumulateLiveEventSummary(deferred.summary,event);
    DeferDetail(deferred.events,event,8192,deferred.summary.omittedEvents);
#else
    (void)event;
#endif
}
void ICommandList::DiscardLiveDeferred() noexcept
{
#ifdef DY_LIVE_PROFILING_ENABLED
    auto& deferred=*m_liveDeferred;
    const auto bytes=deferred.events.capacity()*sizeof(D::LiveEvent)+deferred.links.capacity()*sizeof(D::LiveFlowLink);
    std::vector<D::LiveEvent>().swap(deferred.events);
    std::vector<D::LiveFlowLink>().swap(deferred.links);
    if(bytes) D::ReleaseLiveDetailBytes(bytes);
    deferred.summary={};
#endif
}
void ICommandList::FlushLiveDeferred(D::LiveContext context) noexcept
{
#ifdef DY_LIVE_PROFILING_ENABLED
    auto& deferred=*m_liveDeferred;
    if(deferred.events.empty() && deferred.links.empty() &&
        !deferred.summary.omittedEvents && !deferred.summary.omittedLinks) return;
    D::LiveContextScope restore(context);
    // Publish exact counters and detail together, without locking the frame per command.
    D::RecordLiveCommandBatch(deferred.m_livePosition,deferred.summary,deferred.events,deferred.links);
    deferred.events.clear();
    deferred.links.clear();
    deferred.summary={};
#else
    (void)context;
#endif
}
void ICommandList::RecordLiveInfo(D::LiveEvent event) noexcept
{
#ifdef DY_LIVE_PROFILING_ENABLED
    if(!m_liveDeferred->m_livePosition.commandList||!D::IsLiveCollectorEnabled())return;
    try
    {
        auto context=event.context.frame.frame?event.context:D::GetLiveContext();
        if(!context.frame.frame||context.frame.device!=m_liveDeferred->m_liveContext.frame.device)context=m_liveDeferred->m_liveContext;
        event.context=context;
        if(context.frame.frame && context.frame != m_liveDeferred->m_liveContext.frame)
            D::RecordLiveEvent(event);
        else DeferLiveEvent(event);
    }
    catch(...){}
#else
    (void)event;
#endif
}
void ICommandList::RecordLiveResourceLink(D::LiveId from,D::LiveId to) noexcept
{
#ifdef DY_LIVE_PROFILING_ENABLED
    if(!from||!to||!D::IsLiveCollectorEnabled())return;
    try
    {
        auto& deferred=*m_liveDeferred;
        if(deferred.m_liveContext.frame.frame && deferred.links.size() >= LiveLinkBatchSize)
            FlushLiveDeferred(deferred.m_liveContext);
        DeferDetail(deferred.links,D::LiveFlowLink{from,to},16384,deferred.summary.omittedLinks);
    }
    catch(...){}
#else
    (void)from;(void)to;
#endif
}
void ICommandList::RecordLiveCommand(D::LiveCommandKind kind,std::array<int64_t,5> arguments,
    const void* resource,uint64_t elements,uint64_t bytes,uint64_t operations) noexcept
{
    RecordLiveCommand(kind,arguments,resource,elements,bytes,operations,D::LiveEventKind::Command,false,nullptr);
}
void ICommandList::RecordLiveCommand(D::LiveCommandKind commandKind,std::array<int64_t,5> arguments,
    const void* resource,uint64_t elements,uint64_t bytes,uint64_t operations,D::LiveEventKind kind,bool fullUpload,const char* label) noexcept
{
#ifdef DY_LIVE_PROFILING_ENABLED
    if(!m_liveDeferred->m_livePosition.commandList||m_recordingFailed||!D::IsLiveCollectorEnabled())return;
    try
    {
        auto context=D::GetLiveContext();
        if(!context.frame.frame||context.frame!=m_liveDeferred->m_liveContext.frame)context=m_liveDeferred->m_liveContext;
        D::LiveEvent event;event.id=D::AllocateLiveId();event.context=context;
        event.command=m_liveDeferred->m_livePosition;event.command.index=++m_liveDeferred->m_livePosition.index;
        event.resource=m_liveDeferred->FindObject(resource);event.kind=kind;event.commandKind=commandKind;
        event.stage=kind==D::LiveEventKind::Upload?D::LiveStage::Upload:D::LiveStage::Record;
        event.beginNs=m_liveDeferred->m_liveCommandBeginNs;event.endNs=D::LiveNowNs();event.arguments=arguments;
        event.elements=elements;event.bytes=bytes;event.operations=operations;event.fullUpload=fullUpload;
        for(size_t i=0;label&&label[i]&&i+1<sizeof(event.label);++i)event.label[i]=label[i];
        if(!event.beginNs)event.beginNs=event.endNs;
        DeferLiveEvent(event);
        RecordLiveResourceLink(event.resource,event.id);
        RecordLiveBindings(event.id,commandKind);
        RecordLiveResourceLink(context.batch,event.id);
        RecordLiveResourceLink(m_liveDeferred->m_livePreviousEvent,event.id);
        m_liveDeferred->m_livePreviousEvent=event.id;m_liveDeferred->m_liveCommandBeginNs=0;
    }
    catch(...) {m_liveDeferred->m_liveCommandBeginNs=0;}
#else
    (void)commandKind;(void)arguments;(void)resource;(void)elements;(void)bytes;(void)operations;(void)kind;(void)fullUpload;(void)label;
#endif
}
void ICommandList::RecordLiveBindings(D::LiveId event,D::LiveCommandKind kind) noexcept
{
#ifdef DY_LIVE_PROFILING_ENABLED
    const bool draw=kind==D::LiveCommandKind::Draw||kind==D::LiveCommandKind::DrawIndexed;
    if(!draw&&kind!=D::LiveCommandKind::Dispatch&&kind!=D::LiveCommandKind::DispatchMesh)return;
    RecordLiveResourceLink(m_liveDeferred->FindObject(m_pipeline),event);
    if(draw&&m_pipeline)for(const auto binding:m_pipeline->m_requiredVertexBindings)
    {const auto found=m_vertexBuffers.find(binding);if(found!=m_vertexBuffers.end())RecordLiveResourceLink(m_liveDeferred->FindObject(found->second),event);}
    if(kind==D::LiveCommandKind::DrawIndexed)RecordLiveResourceLink(m_liveDeferred->FindObject(m_indexBuffer),event);
    if(m_resourceSet&&m_resourceSet->GetPipeline()==m_pipeline)
    {
        RecordLiveResourceLink(m_liveDeferred->FindObject(m_resourceSet),event);
        for(uint32_t i=0;i<m_resourceSet->GetBindingCount();++i)
        {const auto& binding=m_resourceSet->GetBindings()[i];
            RecordLiveResourceLink(m_liveDeferred->FindObject(binding.buffer),event);
            RecordLiveResourceLink(m_liveDeferred->FindObject(binding.texture),event);}
    }
#else
    (void)event;(void)kind;
#endif
}
void ICommandList::FinishLiveRecording(D::LiveOutcome outcome,bool terminal) noexcept
{
#ifdef DY_LIVE_PROFILING_ENABLED
    if(!m_liveDeferred->m_livePosition.commandList)return;
    try
    {
        D::LiveContextScope context(m_liveDeferred->m_liveContext);FlushLiveDeferred(m_liveDeferred->m_liveContext);
        if(terminal&&!m_liveDeferred->m_liveTerminal&&!m_liveDeferred->m_liveSubmitted&&m_liveDeferred->m_livePosition.index)
        {
            D::LiveEvent event;event.context=m_liveDeferred->m_liveContext;event.command=m_liveDeferred->m_livePosition;event.kind=D::LiveEventKind::CpuWork;
            event.stage=D::LiveStage::Record;event.outcome=outcome;event.beginNs=event.endNs=D::LiveNowNs();
            const char label[]="Recording ended without submission";std::copy(std::begin(label),std::end(label),event.label);
            D::RecordLiveEvent(event);m_liveDeferred->m_liveTerminal=true;
        }
        if(m_liveDeferred->m_liveRecordFrame.ownsFrame)
        {auto token=m_liveDeferred->m_liveRecordFrame;m_liveDeferred->m_liveRecordFrame={};EndLiveRhiFrame(token,outcome);}
    }
    catch(...){}
#else
    (void)outcome;(void)terminal;
#endif
}
void ICommandList::BeginDebugEvent(const char* name, const DebugLabelColor& color)
try
{
    if(!CanRecordCommands()) return;
    if(!name || !*name || !std::isfinite(color.r) || !std::isfinite(color.g) ||
        !std::isfinite(color.b) || !std::isfinite(color.a)) { m_recordingFailed = true; return; }
    ++m_debugEventDepth;
#ifdef DY_RENDERDOC_ENABLED
    BeginDebugEventNative(name, color);
#endif
}
catch(...) { m_recordingFailed=true; throw; }
void ICommandList::EndDebugEvent()
try
{
    if(!CanRecordCommands()) return;
    if(!m_debugEventDepth) { m_recordingFailed = true; return; }
    --m_debugEventDepth;
#ifdef DY_RENDERDOC_ENABLED
    EndDebugEventNative();
#endif
}
catch(...) { m_recordingFailed=true; throw; }
void ICommandList::InsertDebugMarker(const char* name, const DebugLabelColor& color)
try
{
    if(!CanRecordCommands()) return;
    if(!name || !*name || !std::isfinite(color.r) || !std::isfinite(color.g) ||
        !std::isfinite(color.b) || !std::isfinite(color.a)) { m_recordingFailed = true; return; }
#ifdef DY_RENDERDOC_ENABLED
    InsertDebugMarkerNative(name, color);
#endif
}
catch(...) { m_recordingFailed=true; throw; }
void ICommandList::ResetTimestamps(TimestampQueryHandle query,uint32_t first,uint32_t count)
try
{
    if(!Track(query)) return;
    if(m_rendering || !count || first>=query->GetCount() || count>query->GetCount()-first) {m_recordingFailed=true;return;}
    ResetTimestampsNative(query,first,count);
}
catch(...) { m_recordingFailed=true; throw; }
void ICommandList::WriteTimestamp(TimestampQueryHandle query,uint32_t index)
try
{
    if(!Track(query)) return;
    if(index>=query->GetCount()) {m_recordingFailed=true;return;}
    WriteTimestampNative(query,index);
}
catch(...) { m_recordingFailed=true; throw; }
ICommandList::~ICommandList()
{
    if(m_owner)
    {
        std::lock_guard<std::recursive_mutex> lock(m_owner->m_resourceMutex);
        FinishLiveRecording(m_recordingFailed?D::LiveOutcome::Failed:D::LiveOutcome::Discarded,true);
        m_owner->DiscardLivePrepared(this,D::LiveOutcome::Discarded);
        if(m_preparedNative) m_owner->DiscardCommandListNative(m_preparedNative);
        m_owner->m_recordedCommands.erase(this);
        m_references.clear();
        m_referenceHandles.clear();
        m_sharedReferences.reset();
    }
    DiscardLiveDeferred();
    D::RetireLiveObject(m_liveDeferred->m_livePosition.commandList);
}

bool ICommandList::CanRecordCommands()
{
    if(!m_owner || m_recordingClosed) m_recordingFailed = true;
#ifdef DY_LIVE_PROFILING_ENABLED
    if(!m_recordingFailed&&m_liveDeferred->m_livePosition.commandList&&!m_liveDeferred->m_liveCommandBeginNs)m_liveDeferred->m_liveCommandBeginNs=D::LiveNowNs();
#endif
    return !m_recordingFailed;
}

bool ICommandList::ReplayNative(const std::vector<std::function<bool(ICommandList&)>>& commands)
{
    if(m_liveDeferred->m_liveReplayQuery)
    {ResetTimestampsNative(m_liveDeferred->m_liveReplayQuery,m_liveDeferred->m_liveFirstQuery,2);WriteTimestampNative(m_liveDeferred->m_liveReplayQuery,m_liveDeferred->m_liveFirstQuery);}
    for(const auto& record : commands)
        if(!record(*this)) return false;
    if(m_liveDeferred->m_liveReplayQuery)WriteTimestampNative(m_liveDeferred->m_liveReplayQuery,m_liveDeferred->m_liveFirstQuery+1);
    return !m_recordingFailed && CloseNative();
}

void ICommandList::GlobalBarrier()
try
{
    if(!CanRecordCommands()) return;
    if(m_rendering) { m_recordingFailed = true; return; }
    GlobalBarrierNative();
}
catch(...) { m_recordingFailed=true; throw; }

bool ICommandList::Track(const void* handle)
try
{
    if(!CanRecordCommands()) return false;
    // Destroy* 전에 보유한 참조는 목록이 끝날 때까지 유지한다.
    if(m_referenceHandles.count(handle) || (m_sharedReferences && m_sharedReferences->handles.count(handle))) return true;
    // 자원 소유권과 스왑체인 세대는 같은 잠금 안에서 가져온다.
    // 명령 목록 자체는 기록 중인 스레드 하나만 변경한다.
    std::lock_guard<std::recursive_mutex> lock(m_owner->m_resourceMutex);
    auto reference = m_owner->Reference(handle);
    if(!reference) { m_recordingFailed = true; return false; }
    if(m_owner->m_borrowedTextures.count(handle)) m_imageGeneration=m_owner->m_imageGeneration;
    m_references.push_back(std::move(reference));
    try { m_referenceHandles.insert(handle); }
    catch(...) { m_references.pop_back(); throw; }
    return true;
}
catch(...) { m_recordingFailed=true; throw; }

void ICommandList::ResourceBarrier(const ResourceBarrierDesc* barriers, uint32_t count)
try
{
    if(!CanRecordCommands()) return;
    if(count && !barriers) { m_recordingFailed = true; return; }
    if(count) m_requiredResourceSets.clear();
    for(uint32_t i = 0; i < count; ++i)
    {
        const auto& barrier = barriers[i];
        if((barrier.buffer != nullptr) == (barrier.texture != nullptr))
        { m_recordingFailed = true; return; }
        if(!Track(barrier.buffer ? static_cast<const void*>(barrier.buffer) : barrier.texture)) return;

        const uintptr_t object=reinterpret_cast<uintptr_t>(barrier.buffer?static_cast<const void*>(barrier.buffer):barrier.texture);
        uint32_t mip=0,mips=1,layer=0,layers=1;
        if(barrier.buffer)
        {
            if(!IsBufferStateAllowed(barrier.buffer->GetDesc(),barrier.after))
            {m_recordingFailed=true;return;}
        }
        else if(!IsTextureStateAllowed(barrier.texture->GetDesc(),barrier.after) ||
            !ResolveSubresources(barrier.texture,barrier.subresources,mip,mips,layer,layers))
        {m_recordingFailed=true;return;}
        for(uint32_t l=layer;l<layer+layers;++l)for(uint32_t m=mip;m<mip+mips;++m)
        {
            const auto key=std::make_tuple(object,m,l);
            m_stateOperations.push_back({object,m,l,barrier.before,barrier.after,true});
            m_recordedStates[key]=barrier.after;
        }

    }
    ResourceBarrierNative(barriers, count);
}
catch(...) { m_recordingFailed=true; throw; }


void ICommandList::RequireState(const void* resource,uint32_t mip,uint32_t layer,ResourceState state)
try
{
    const auto key=std::make_tuple(reinterpret_cast<uintptr_t>(resource),mip,layer);
    // 이 목록의 앞선 검사나 배리어가 같은 상태를 이미 보장하면 중복 검사만 합친다.
    const auto recorded=m_recordedStates.find(key);
    if(recorded!=m_recordedStates.end() && recorded->second==state) return;
    m_stateOperations.push_back({reinterpret_cast<uintptr_t>(resource),mip,layer,state,state,false});
    m_recordedStates[key]=state;
}
catch(...) { m_recordingFailed=true; throw; }
void ICommandList::BeginRendering(const RenderingDesc& desc)
try
{
    if(!CanRecordCommands())return;
    if(m_rendering || (desc.colorAttachmentCount && !desc.colorAttachments) ||
        (!desc.colorAttachmentCount && !desc.depthStencilAttachment)) {m_recordingFailed=true;return;}
    uint32_t width=0,height=0;
    const auto attachment=[&](TextureHandle texture,uint32_t mip,uint32_t layer,ResourceState state,LoadOp load,StoreOp store) {
        if(!Track(texture))return false;
        const auto& d=texture->GetDesc();
        if(mip>=d.mipLevels || layer>=d.depthOrArraySize ||
            load<LoadOp::Load || load>LoadOp::Discard || store<StoreOp::Store || store>StoreOp::Discard ||
            !IsTextureStateAllowed(d,state))return false;
        const uint32_t w=std::max(1u,d.width>>mip),h=std::max(1u,d.height>>mip);
        if(width && (width!=w || height!=h))return false;
        width=w;height=h;RequireState(texture,mip,layer,state);return true;
    };
    std::vector<ColorAttachment> colors;
    if(desc.colorAttachmentCount)colors.assign(desc.colorAttachments,desc.colorAttachments+desc.colorAttachmentCount);
    for(uint32_t i=0;i<desc.colorAttachmentCount;++i)
    {
        auto& color=colors[i];
        if(!attachment(color.texture,color.mipLevel,color.arrayLayer,ResourceState::RenderTarget,color.loadOp,color.storeOp))
        {m_recordingFailed=true;return;}
        if(color.loadOp==LoadOp::Clear)
        {
            const auto format=color.texture->GetDesc().format;
            const uint32_t channels=(format==Format::R16_UINT || format==Format::R32_UINT || format==Format::R32_FLOAT) ? 1u :
                format==Format::R32G32_FLOAT ? 2u : format==Format::R32G32B32_FLOAT ? 3u : 4u;
            for(uint32_t channel=0;channel<channels;++channel)
                if(!std::isfinite(color.clearColor[channel])){m_recordingFailed=true;return;}
            // 없는 채널은 계약상 사용하지 않는다. native에도 무의미한 NaN/Inf를 전달하지 않는다.
            for(uint32_t channel=channels;channel<4;++channel)color.clearColor[channel]=0;
            if(format==Format::R16_UINT || format==Format::R32_UINT)
            {
                const double value=color.clearColor[0];
                const double maximum=format==Format::R16_UINT ? 65535.0 : 4294967295.0;
                if(value<0 || value>maximum || std::trunc(value)!=value)
                {
                    m_owner->ReportDiagnostic(IDevice::DiagnosticSeverity::Error,"BeginRendering: UINT clear requires a nonnegative integral value within the target format range. Values are not rounded, clamped or reinterpreted.");
                    m_recordingFailed=true;return;
                }
            }
        }
    }
    if(desc.depthStencilAttachment)
    {
        const auto& depth=*desc.depthStencilAttachment;
        if((depth.state!=ResourceState::DepthWrite && depth.state!=ResourceState::DepthRead) ||
            !attachment(depth.texture,depth.mipLevel,depth.arrayLayer,depth.state,depth.depthLoadOp,depth.depthStoreOp) ||
            (depth.depthLoadOp==LoadOp::Clear &&
                (!std::isfinite(depth.clearDepth) || depth.clearDepth<0 || depth.clearDepth>1 || depth.state!=ResourceState::DepthWrite)))
        {m_recordingFailed=true;return;}
        if(HasStencil(depth.texture->GetDesc().format) &&
            (depth.stencilLoadOp<LoadOp::Load || depth.stencilLoadOp>LoadOp::Discard ||
             depth.stencilStoreOp<StoreOp::Store || depth.stencilStoreOp>StoreOp::Discard))
        {m_recordingFailed=true;return;}
        if(HasStencil(depth.texture->GetDesc().format) && depth.stencilLoadOp==LoadOp::Clear &&
            (depth.clearStencil>255 || depth.state!=ResourceState::DepthWrite))
        {
            m_owner->ReportDiagnostic(IDevice::DiagnosticSeverity::Error,"BeginRendering: stencil clear requires DepthWrite and a value in [0,255]; high bits are not silently discarded.");
            m_recordingFailed=true;return;
        }
    }
    m_colorFormats.clear();
    for(uint32_t i=0;i<desc.colorAttachmentCount;++i)
        m_colorFormats.push_back(desc.colorAttachments[i].texture->GetDesc().format);
    m_depthStencilFormat=desc.depthStencilAttachment ? desc.depthStencilAttachment->texture->GetDesc().format : Format::Unknown;
    DepthStencilAttachment depthCopy;
    const auto* nativeDepth=desc.depthStencilAttachment;
    if(nativeDepth && m_depthStencilFormat==Format::D32_FLOAT &&
        (nativeDepth->stencilLoadOp!=LoadOp::Undefined || nativeDepth->stencilStoreOp!=StoreOp::Undefined || nativeDepth->clearStencil!=0))
    {
        m_owner->ReportDiagnostic(IDevice::DiagnosticSeverity::Warning,
            "BeginRendering: D32_FLOAT has no stencil aspect; stencil load/store/clear settings are ignored.");
        depthCopy=*nativeDepth;
        depthCopy.stencilLoadOp=LoadOp::Undefined;
        depthCopy.stencilStoreOp=StoreOp::Undefined;
        depthCopy.clearStencil=0;
        nativeDepth=&depthCopy;
    }
    BeginRenderingNative({colors.data(),desc.colorAttachmentCount,nativeDepth});
}
catch(...) { m_recordingFailed=true; throw; }

void ICommandList::EndRendering() try
{ if(CanRecordCommands()) EndRenderingNative(); }
catch(...) { m_recordingFailed=true; throw; }
void ICommandList::BindGraphicsPipeline(PipelineHandle pipeline) try
{if(!Track(pipeline))return;if(pipeline->IsCompute()){m_recordingFailed=true;return;}m_resourceSet=nullptr;BindGraphicsPipelineNative(pipeline);}
catch(...) { m_recordingFailed=true; throw; }
void ICommandList::BindComputePipeline(PipelineHandle pipeline) try
{if(!Track(pipeline))return;if(m_rendering||!pipeline->IsCompute()){m_recordingFailed=true;return;}m_resourceSet=nullptr;BindComputePipelineNative(pipeline);}
catch(...) { m_recordingFailed=true; throw; }
void ICommandList::Dispatch(uint32_t x,uint32_t y,uint32_t z) try
{if(!CanRecordCommands())return;if(m_rendering||!m_pipeline||!m_pipeline->IsCompute()||!x||!y||!z){m_recordingFailed=true;return;}if(ValidateBindings(false,true))DispatchNative(x,y,z);}
catch(...) { m_recordingFailed=true; throw; }
void ICommandList::DispatchIndirect(BufferHandle arguments, uint32_t offset)
try
{
    if(!CanRecordCommands()) return;
    if(m_rendering || !m_owner->Supports(Feature::IndirectDispatch))
    {
        m_owner->ReportDiagnostic(IDevice::DiagnosticSeverity::Error,
            "DispatchIndirect: requires indirect dispatch support and must be recorded outside rendering.");
        m_recordingFailed=true;return;
    }
    if(ValidateBindings(false,true) &&
        ValidateIndirectArguments(arguments,offset,sizeof(DispatchIndirectArguments)))
        DispatchIndirectNative(arguments,offset);
}
catch(...) { m_recordingFailed=true; throw; }

void ICommandList::BindResourceSet(ResourceSetHandle resources)
try
{
    if(!Track(resources))return;
    if(!m_pipeline || resources->GetPipeline()!=m_pipeline || (!m_pipeline->IsCompute() && !m_rendering))
    {m_recordingFailed=true;return;}
    if(!RequireResourceSetStates(resources))return;
    m_resourceSet=resources;
    BindResourceSetNative(resources);
}
catch(...) { m_recordingFailed=true; throw; }

bool ICommandList::RequireResourceSetStates(ResourceSetHandle resources)
{
    // 세트는 불변이므로 배리어 사이에서는 바인딩과 전체 서브리소스를 다시 순회하지 않는다.
    if(m_requiredResourceSets.count(resources)) return true;
    for(uint32_t i=0;i<resources->GetBindingCount();++i)
    {
        const auto& binding=resources->GetBindings()[i];
        const auto* layout=FindLayoutBinding(m_pipeline->GetLayout(),binding.binding);
        if(!layout){m_recordingFailed=true;return false;}
        if(binding.buffer)
        {
            const auto state=layout->type==ResourceBindingType::ConstantBuffer?ResourceState::ConstantBuffer:
                layout->type==ResourceBindingType::ReadWriteStorageBuffer?ResourceState::UnorderedAccess:ResourceState::ShaderResource;
            RequireState(binding.buffer,0,0,state);
        }
        if(binding.texture)
        {
            uint32_t mip,mips,layer,layers;
            if(!ResolveBindingSubresources(binding.texture,binding,layout->type,mip,mips,layer,layers))
            {m_recordingFailed=true;return false;}
            for(uint32_t l=layer;l<layer+layers;++l)for(uint32_t m=mip;m<mip+mips;++m)
                RequireState(binding.texture,m,l,layout->type==ResourceBindingType::StorageTexture?
                    ResourceState::UnorderedAccess:ResourceState::ShaderResource);
        }
    }
    m_requiredResourceSets.insert(resources);
    return true;
}
void ICommandList::BindVertexBuffer(uint32_t binding,BufferHandle buffer,uint32_t offset)
try
{
    if(!Track(buffer))return;
    if(!m_rendering || !m_pipeline || m_pipeline->IsCompute() || offset>=buffer->GetDesc().size || !HasUsage(buffer->GetDesc().usage,BufferUsage::Vertex))
    {m_recordingFailed=true;return;}
    RequireState(buffer,0,0,ResourceState::VertexBuffer);
    m_vertexBuffers[binding]=buffer;
    BindVertexBufferNative(binding,buffer,offset);
}
catch(...) { m_recordingFailed=true; throw; }
void ICommandList::BindIndexBuffer(BufferHandle buffer,Format format,uint32_t offset)
try
{
    if(!Track(buffer))return;
    if(!m_rendering || (format!=Format::R16_UINT && format!=Format::R32_UINT) || offset>=buffer->GetDesc().size ||
        offset%FormatSize(format) || !HasUsage(buffer->GetDesc().usage,BufferUsage::Index))
    {m_recordingFailed=true;return;}
    RequireState(buffer,0,0,ResourceState::IndexBuffer);
    BindIndexBufferNative(buffer,format,offset);
}
catch(...) { m_recordingFailed=true; throw; }
void ICommandList::SetInlineConstants(uint32_t offset, uint32_t size, const void* data)
try
{
    if(!CanRecordCommands()) return;
    if(!data || !size || !m_pipeline || offset%4 || size%4 || offset>m_pipeline->GetLayout().inlineConstantSize ||
        size>m_pipeline->GetLayout().inlineConstantSize-offset) { m_recordingFailed = true; return; }
    SetInlineConstantsNative(offset, size, data);
}
catch(...) { m_recordingFailed=true; throw; }
void ICommandList::SetViewport(const Viewport& viewport) try
{
    if(!CanRecordCommands())return;
    if(!std::isfinite(viewport.x+viewport.y+viewport.width+viewport.height+viewport.minDepth+viewport.maxDepth) ||
        viewport.width<=0 || viewport.height<=0 || viewport.minDepth<0 || viewport.maxDepth>1 || viewport.minDepth>viewport.maxDepth)
    {m_recordingFailed=true;return;}
    SetViewportNative(viewport);
}
catch(...) { m_recordingFailed=true; throw; }
void ICommandList::SetScissor(const Rect& rect) try
{
    if(!CanRecordCommands())return;
    if(rect.x<0 || rect.y<0 || !rect.width || !rect.height) {m_recordingFailed=true;return;}
    SetScissorNative(rect);
}
catch(...) { m_recordingFailed=true; throw; }
void ICommandList::SetStencilReference(uint32_t reference)
try
{
    if(!CanRecordCommands())return;
    if(reference>255)
    {
        m_owner->ReportDiagnostic(IDevice::DiagnosticSeverity::Error,"SetStencilReference: the 8-bit stencil reference must be in [0,255]; high bits are not silently discarded.");
        m_recordingFailed=true;return;
    }
    SetStencilReferenceNative(reference);
}
catch(...) { m_recordingFailed=true; throw; }

bool ICommandList::ValidateBindings(bool indexed, bool compute)
{
    const auto fail=[&](const char* message) {
        m_owner->ReportDiagnostic(IDevice::DiagnosticSeverity::Error,message);
        m_recordingFailed=true;
        return false;
    };
    if(!m_pipeline || m_pipeline->IsCompute()!=compute || m_pipeline->IsMesh())
        return fail("Draw/Dispatch: bind a pipeline of the matching type before execution.");
    if(!compute)
    {
        if(!m_rendering || m_colorFormats!=m_pipeline->m_colorFormats || m_depthStencilFormat!=m_pipeline->m_depthStencilFormat)
            return fail("Draw: rendering attachment formats must match the graphics pipeline.");
        for(uint32_t binding:m_pipeline->m_requiredVertexBindings)
        {
            const auto found=m_vertexBuffers.find(binding);
            if(found==m_vertexBuffers.end())return fail("Draw: a vertex attribute binding has no vertex buffer.");
            RequireState(found->second,0,0,ResourceState::VertexBuffer);
        }
        if(indexed)
        {
            if(!m_indexBuffer)return fail("DrawIndexed: no index buffer is bound.");
            RequireState(m_indexBuffer,0,0,ResourceState::IndexBuffer);
        }
    }
    const auto& layout=m_pipeline->GetLayout();
    bool requiresSet=false;
    for(uint32_t i=0;i<layout.bindingCount;++i)
        if(layout.bindings[i].type!=ResourceBindingType::StaticSampler)requiresSet=true;
    if(requiresSet && (!m_resourceSet || m_resourceSet->GetPipeline()!=m_pipeline))
        return fail("Draw/Dispatch: bind a complete ResourceSet for the current pipeline after selecting that pipeline.");
    return !m_resourceSet || RequireResourceSetStates(m_resourceSet);
}
bool ICommandList::ValidateIndirectArguments(BufferHandle arguments, uint32_t offset, uint32_t size)
{
    if(m_pipeline && m_pipeline->m_topology==PrimitiveTopology::PatchList &&
        !m_owner->Supports(Feature::IndirectTessellation))
    {
        m_owner->ReportDiagnostic(IDevice::DiagnosticSeverity::Error,
            "Indirect drawing with a PatchList pipeline requires Feature::IndirectTessellation.");
        m_recordingFailed=true;return false;
    }
    if(!Track(arguments)) return false;
    const auto& desc=arguments->GetDesc();
    if(!HasUsage(desc.usage,BufferUsage::Indirect) || offset%4 ||
        offset>desc.size || size>desc.size-offset)
    {
        m_owner->ReportDiagnostic(IDevice::DiagnosticSeverity::Error,
            "Indirect command: requires BufferUsage::Indirect, a 4-byte aligned offset and a complete argument structure within the buffer.");
        m_recordingFailed=true;return false;
    }
    RequireState(arguments,0,0,ResourceState::IndirectArgument);
    return true;
}
bool ICommandList::ValidateMeshBindings()
{
    const auto fail=[&](const char* message) {
        m_owner->ReportDiagnostic(IDevice::DiagnosticSeverity::Error,message);
        m_recordingFailed=true;
        return false;
    };
    if(!m_pipeline || !m_pipeline->IsMesh())
        return fail("DispatchMesh: bind a mesh pipeline before execution.");
    if(!m_rendering || m_colorFormats!=m_pipeline->m_colorFormats || m_depthStencilFormat!=m_pipeline->m_depthStencilFormat)
        return fail("DispatchMesh: rendering attachment formats must match the mesh pipeline.");
    const auto& layout=m_pipeline->GetLayout();
    bool requiresSet=false;
    for(uint32_t i=0;i<layout.bindingCount;++i)
        if(layout.bindings[i].type!=ResourceBindingType::StaticSampler)requiresSet=true;
    if(requiresSet && (!m_resourceSet || m_resourceSet->GetPipeline()!=m_pipeline))
        return fail("DispatchMesh: bind a complete ResourceSet for the current pipeline after selecting that pipeline.");
    return !m_resourceSet || RequireResourceSetStates(m_resourceSet);
}
void ICommandList::DrawInstanced(uint32_t vertices, uint32_t instances, uint32_t firstVertex, uint32_t firstInstance)
try
{ if(CanRecordCommands() && ValidateBindings(false,false)) DrawInstancedNative(vertices, instances, firstVertex, firstInstance); }
catch(...) { m_recordingFailed=true; throw; }
void ICommandList::DrawIndexedInstanced(uint32_t indices, uint32_t instances, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
try
{ if(CanRecordCommands() && ValidateBindings(true,false)) DrawIndexedInstancedNative(indices, instances, firstIndex, vertexOffset, firstInstance); }
catch(...) { m_recordingFailed=true; throw; }
void ICommandList::DrawIndirect(BufferHandle arguments, uint32_t offset)
try
{
    if(!CanRecordCommands()) return;
    if(!m_owner->Supports(Feature::IndirectDraw))
    {
        m_owner->ReportDiagnostic(IDevice::DiagnosticSeverity::Error,
            "DrawIndirect: indirect drawing is not supported by this device.");
        m_recordingFailed=true;return;
    }
    if(ValidateBindings(false,false) &&
        ValidateIndirectArguments(arguments,offset,sizeof(DrawIndirectArguments)))
        DrawIndirectNative(arguments,offset);
}
catch(...) { m_recordingFailed=true; throw; }
void ICommandList::DrawIndexedIndirect(BufferHandle arguments, uint32_t offset)
try
{
    if(!CanRecordCommands()) return;
    if(!m_owner->Supports(Feature::IndirectDraw))
    {
        m_owner->ReportDiagnostic(IDevice::DiagnosticSeverity::Error,
            "DrawIndexedIndirect: indirect drawing is not supported by this device.");
        m_recordingFailed=true;return;
    }
    if(ValidateBindings(true,false) &&
        ValidateIndirectArguments(arguments,offset,sizeof(DrawIndexedIndirectArguments)))
        DrawIndexedIndirectNative(arguments,offset);
}
catch(...) { m_recordingFailed=true; throw; }

void ICommandList::DispatchMesh(uint32_t x, uint32_t y, uint32_t z)
try
{
    if(!CanRecordCommands()) return;
    if(!x || !y || !z) { m_recordingFailed=true; return; }
    if(ValidateMeshBindings()) DispatchMeshNative(x, y, z);
}
catch(...) { m_recordingFailed=true; throw; }

bool ICommandList::Close()
try
{
    if(m_recordingClosed) { m_recordingFailed = true; return false; }
    if(m_debugEventDepth) m_recordingFailed = true;
    const bool nativeClosed = CloseNative();
    m_recordingClosed = true;
    m_recordingFailed = m_recordingFailed || !nativeClosed;
    if(m_recordingFailed && m_owner)m_owner->ReportDiagnostic(IDevice::DiagnosticSeverity::Error,"Close: command recording failed. Invalid commands will not be submitted.");
    FinishLiveRecording(m_recordingFailed?D::LiveOutcome::Failed:D::LiveOutcome::Observed,m_recordingFailed);
    return !m_recordingFailed;
}
catch(...) { m_recordingFailed=true;FinishLiveRecording(D::LiveOutcome::Failed,true); throw; }
}


namespace dyf::RHI
{
class RecordedCommandList final : public ICommandList
{
    void GlobalBarrierNative() override
    { m_commands.push_back([](ICommandList& native) { native.GlobalBarrierNative(); return true; });
      RecordLiveCommand(D::LiveCommandKind::Barrier,{},nullptr,0,0,1,D::LiveEventKind::Command,false,"Global barrier"); }
    void BeginDebugEventNative(const char* name,const DebugLabelColor& color) override
    {
#ifdef DY_RENDERDOC_ENABLED
        m_commands.push_back([name=std::string(name),color](ICommandList& n){n.BeginDebugEventNative(name.c_str(),color);return true;});
#else
        (void)name;(void)color;
#endif
    }
    void EndDebugEventNative() override
    {
#ifdef DY_RENDERDOC_ENABLED
        m_commands.push_back([](ICommandList& n){n.EndDebugEventNative();return true;});
#endif
    }
    void InsertDebugMarkerNative(const char* name,const DebugLabelColor& color) override
    {
#ifdef DY_RENDERDOC_ENABLED
        m_commands.push_back([name=std::string(name),color](ICommandList& n){n.InsertDebugMarkerNative(name.c_str(),color);return true;});
#else
        (void)name;(void)color;
#endif
    }
    void ResourceBarrierNative(const ResourceBarrierDesc* values, uint32_t count) override
    {
        std::vector<ResourceBarrierDesc> copy;
        if(count) copy.assign(values,values+count);
        m_commands.push_back([copy=std::move(copy)](ICommandList& native) { native.ResourceBarrierNative(copy.data(),static_cast<uint32_t>(copy.size())); return true; });
#ifdef DY_LIVE_PROFILING_ENABLED
        if(D::IsLiveCollectorEnabled())for(uint32_t i=0;i<count;++i)RecordLiveCommand(D::LiveCommandKind::Barrier,
            {static_cast<int64_t>(values[i].before),static_cast<int64_t>(values[i].after),values[i].subresources.firstMipLevel,
             values[i].subresources.firstArrayLayer,values[i].subresources.mipLevelCount},
            values[i].buffer?static_cast<const void*>(values[i].buffer):values[i].texture,
            values[i].subresources.arrayLayerCount,0,1,D::LiveEventKind::Command,false,"Resource barrier");
#endif
    }
    void BeginRenderingNative(const RenderingDesc& desc) override
    {
        if(m_rendering) { m_recordingFailed=true; return; }
        m_rendering=true;
        const auto pipeline=m_pipeline && !m_pipeline->IsCompute() ? m_pipeline : nullptr;
        m_pipeline=pipeline;
        std::vector<ColorAttachment> colors;
        if(desc.colorAttachmentCount) colors.assign(desc.colorAttachments,desc.colorAttachments+desc.colorAttachmentCount);
        const bool hasDepth=desc.depthStencilAttachment!=nullptr;
        const DepthStencilAttachment depth=hasDepth?*desc.depthStencilAttachment:DepthStencilAttachment{};
        m_commands.push_back([colors=std::move(colors),depth,hasDepth,pipeline](ICommandList& native) {
            native.BeginRenderingNative({colors.data(),static_cast<uint32_t>(colors.size()),hasDepth?&depth:nullptr});
            if(pipeline)native.BindGraphicsPipelineNative(pipeline);
            return true;
        });
        RecordLiveCommand(D::LiveCommandKind::None,{desc.colorAttachmentCount},
            desc.colorAttachmentCount?static_cast<const void*>(desc.colorAttachments[0].texture):desc.depthStencilAttachment->texture,
            0,0,1,D::LiveEventKind::Command,false,"Begin rendering");
    }
    void EndRenderingNative() override
    {
        if(!m_rendering) { m_recordingFailed=true; return; }
        m_rendering=false;
        m_pipeline=nullptr;
        m_resourceSet=nullptr;
        m_vertexBuffers.clear();
        m_indexBuffer=nullptr;
        m_viewport=m_scissor=false;
        m_commands.push_back([](ICommandList& n){n.EndRenderingNative();return true;});
        RecordLiveCommand(D::LiveCommandKind::None,{},nullptr,0,0,1,D::LiveEventKind::Command,false,"End rendering");
    }
    void BindComputePipelineNative(PipelineHandle pipeline) override
    {
        m_pipeline=pipeline;
        m_commands.push_back([=](ICommandList& n){n.BindComputePipelineNative(pipeline);return true;});
    }
    void ResetTimestampsNative(TimestampQueryHandle query,uint32_t first,uint32_t count) override
    {m_commands.push_back([=](ICommandList& n){n.ResetTimestampsNative(query,first,count);return true;});
     RecordLiveCommand(D::LiveCommandKind::None,{first,count},query);}
    void WriteTimestampNative(TimestampQueryHandle query,uint32_t index) override
    {m_commands.push_back([=](ICommandList& n){n.WriteTimestampNative(query,index);return true;});
     RecordLiveCommand(D::LiveCommandKind::None,{index},query);}
    void DispatchNative(uint32_t x,uint32_t y,uint32_t z) override
    {m_commands.push_back([=](ICommandList& n){n.DispatchNative(x,y,z);return true;});
     RecordLiveCommand(D::LiveCommandKind::Dispatch,{x,y,z});}
    void DispatchIndirectNative(BufferHandle arguments,uint32_t offset) override
    {m_commands.push_back([=](ICommandList& n){n.DispatchIndirectNative(arguments,offset);return true;});}
    void BindGraphicsPipelineNative(PipelineHandle pipeline) override
    {
        m_pipeline=pipeline;
        // A graph may select its pipeline before the callback opens rendering.
        if(m_rendering)m_commands.push_back([=](ICommandList& n){n.BindGraphicsPipelineNative(pipeline);return true;});
        RecordLiveCommand(D::LiveCommandKind::Bind,{},pipeline,0,0,1,D::LiveEventKind::Command,false,"Graphics pipeline");
    }
    void BindResourceSetNative(ResourceSetHandle resources) override
    {
        m_commands.push_back([=](ICommandList& n){n.BindResourceSetNative(resources);return true;});
#ifdef DY_LIVE_PROFILING_ENABLED
        // A no-op collector would still evaluate the per-binding object lookups.
        if(D::IsLiveCollectorEnabled())
        {
            RecordLiveCommand(D::LiveCommandKind::Bind,{},resources,resources->GetBindingCount(),0,1,D::LiveEventKind::Command,false,"Resource set");
            for(uint32_t i=0;i<resources->GetBindingCount();++i)
            {
                const auto& binding=resources->GetBindings()[i];
                RecordLiveResourceLink(D::FindLiveObject(binding.buffer),D::FindLiveObject(resources));
                RecordLiveResourceLink(D::FindLiveObject(binding.texture),D::FindLiveObject(resources));
            }
        }
#endif
    }
    void BindVertexBufferNative(uint32_t binding, BufferHandle buffer, uint32_t offset) override
    { m_commands.push_back([=](ICommandList& n){n.BindVertexBufferNative(binding,buffer,offset);return true;});
      RecordLiveCommand(D::LiveCommandKind::Bind,{binding,offset},buffer,0,0,1,D::LiveEventKind::Command,false,"Vertex buffer"); }
    void BindIndexBufferNative(BufferHandle buffer, Format format, uint32_t offset) override
    {
        m_indexBuffer=buffer;
        m_commands.push_back([=](ICommandList& n){n.BindIndexBufferNative(buffer,format,offset);return true;});
        RecordLiveCommand(D::LiveCommandKind::Bind,{static_cast<int64_t>(format),offset},buffer,0,0,1,D::LiveEventKind::Command,false,"Index buffer");
    }
    void SetInlineConstantsNative(uint32_t offset,uint32_t size,const void* data) override
    {
        const auto* first=static_cast<const uint8_t*>(data);
        std::vector<uint8_t> bytes(first,first+size);
        m_commands.push_back([offset,bytes=std::move(bytes)](ICommandList& n){n.SetInlineConstantsNative(offset,static_cast<uint32_t>(bytes.size()),bytes.data());return true;});
        RecordLiveCommand(D::LiveCommandKind::Bind,{offset,size},nullptr,0,size,1,D::LiveEventKind::Command,false,"Inline constants");
    }
    void SetViewportNative(const Viewport& viewport) override
    { m_viewport=true; m_commands.push_back([=](ICommandList& n){n.SetViewportNative(viewport);return true;});
      RecordLiveCommand(D::LiveCommandKind::None,{},nullptr,0,0,1,D::LiveEventKind::Command,false,"Viewport"); }
    void SetScissorNative(const Rect& rect) override
    { m_scissor=true; m_commands.push_back([=](ICommandList& n){n.SetScissorNative(rect);return true;});
      RecordLiveCommand(D::LiveCommandKind::None,{rect.x,rect.y,rect.width,rect.height},nullptr,0,0,1,D::LiveEventKind::Command,false,"Scissor"); }
    void SetStencilReferenceNative(uint32_t reference) override
    { m_commands.push_back([=](ICommandList& n){n.SetStencilReferenceNative(reference);return true;});
      RecordLiveCommand(D::LiveCommandKind::Bind,{reference},nullptr,0,0,1,D::LiveEventKind::Command,false,"Stencil reference"); }
    void DrawInstancedNative(uint32_t vertices,uint32_t instances,uint32_t firstVertex,uint32_t firstInstance) override
    {
        if(!m_rendering || !m_pipeline || !m_viewport || !m_scissor) {m_recordingFailed=true;return;}
        m_commands.push_back([=](ICommandList& n){n.DrawInstancedNative(vertices,instances,firstVertex,firstInstance);return true;});
        RecordLiveCommand(D::LiveCommandKind::Draw,{vertices,instances,firstVertex,firstInstance},nullptr,uint64_t(vertices)*instances);
    }
    void DrawIndexedInstancedNative(uint32_t indices,uint32_t instances,uint32_t firstIndex,int32_t vertexOffset,uint32_t firstInstance) override
    {
        if(!m_rendering || !m_pipeline || !m_indexBuffer || !m_viewport || !m_scissor) {m_recordingFailed=true;return;}
        m_commands.push_back([=](ICommandList& n){n.DrawIndexedInstancedNative(indices,instances,firstIndex,vertexOffset,firstInstance);return true;});
        RecordLiveCommand(D::LiveCommandKind::DrawIndexed,{indices,instances,firstIndex,vertexOffset,firstInstance},nullptr,uint64_t(indices)*instances);
    }
    void DrawIndirectNative(BufferHandle arguments,uint32_t offset) override
    {
        if(!m_rendering || !m_pipeline || !m_viewport || !m_scissor) {m_recordingFailed=true;return;}
        m_commands.push_back([=](ICommandList& n){n.DrawIndirectNative(arguments,offset);return true;});
    }
    void DrawIndexedIndirectNative(BufferHandle arguments,uint32_t offset) override
    {
        if(!m_rendering || !m_pipeline || !m_indexBuffer || !m_viewport || !m_scissor) {m_recordingFailed=true;return;}
        m_commands.push_back([=](ICommandList& n){n.DrawIndexedIndirectNative(arguments,offset);return true;});
    }
    void DispatchMeshNative(uint32_t x,uint32_t y,uint32_t z) override
    {
        if(!m_rendering || !m_pipeline || !m_viewport || !m_scissor) {m_recordingFailed=true;return;}
        m_commands.push_back([=](ICommandList& n){n.DispatchMeshNative(x,y,z);return true;});
        RecordLiveCommand(D::LiveCommandKind::DispatchMesh,{x,y,z});
    }
    bool CloseNative() override {return !m_rendering && !m_recordingFailed;}
};
ICommandList* ICommandList::CreateRecorded() {return new RecordedCommandList();}
}
