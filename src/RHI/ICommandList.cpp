#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/Query.h"
#include "dyf/RHI/IDevice.h"
#include "Validation.h"
#include "../Platform/Profiler/RhiProfiler.h"
#include "../Platform/Profiler/CollectorInternal.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>

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
constexpr bool StateSummaryContracts()
{
    CommandStateSummary first(ResourceState::ShaderResource), second(ResourceState::VertexBuffer);
    return first.Append(ResourceState::ShaderResource,ResourceState::ShaderResource,false) &&
        first.Append(ResourceState::ShaderResource,ResourceState::CopyDestination,true) &&
        second.Append(ResourceState::VertexBuffer,ResourceState::VertexBuffer,false) &&
        first.Append(ResourceState::CopyDestination,ResourceState::CopyDestination,false) &&
        first.Append(ResourceState::CopyDestination,ResourceState::ShaderResource,true) &&
        first.initial==ResourceState::ShaderResource && first.final==first.initial && first.writes &&
        second.initial==ResourceState::VertexBuffer && second.final==second.initial && !second.writes &&
        !first.Append(ResourceState::CopyDestination,ResourceState::VertexBuffer,true) &&
        !second.Append(ResourceState::CopyDestination,ResourceState::CopyDestination,false);
}
static_assert(StateSummaryContracts());

constexpr bool CopyRangeFits(uint32_t bufferSize,uint32_t offset,uint32_t size)
{
    return size && offset%4==0 && size%4==0 && uint64_t(offset)+size<=bufferSize;
}
static_assert(CopyRangeFits(16,4,12));
static_assert(!CopyRangeFits(16,8,12));
static_assert(!CopyRangeFits(16,0,0));
static_assert(!CopyRangeFits(16,2,4));
static_assert(!CopyRangeFits(16,0,2));
static_assert(!CopyRangeFits(UINT32_MAX,UINT32_MAX-3,8));

constexpr bool IndirectArgumentsFit(uint32_t bufferSize,uint32_t offset,uint32_t argumentSize,
    uint32_t drawCount,uint32_t stride)
{
    return offset%4==0 && stride%4==0 && stride>=argumentSize && offset<=bufferSize &&
        (!drawCount || uint64_t(drawCount-1)*stride+argumentSize<=uint64_t(bufferSize)-offset);
}
static_assert(IndirectArgumentsFit(40,0,20,2,20));
static_assert(IndirectArgumentsFit(48,4,20,2,24));
static_assert(!IndirectArgumentsFit(47,4,20,2,24));
static_assert(!IndirectArgumentsFit(UINT32_MAX,0,20,UINT32_MAX,20));
static_assert(!IndirectArgumentsFit(UINT32_MAX,UINT32_MAX-3,20,2,UINT32_MAX-3));
static_assert(!IndirectArgumentsFit(40,0,20,1,16));
static_assert(!IndirectArgumentsFit(40,0,20,1,22));
static_assert(!IndirectArgumentsFit(40,2,20,1,20));
static_assert(IndirectArgumentsFit(40,40,20,0,20));
static_assert(!IndirectArgumentsFit(40,44,20,0,20));
static_assert(!IndirectArgumentsFit(40,40,20,0,16));

constexpr bool RectContains(const Rect& bounds,const Rect& rect)
{
    return bounds.x>=0 && bounds.y>=0 && rect.x>=bounds.x && rect.y>=bounds.y &&
        rect.width && rect.height &&
        uint64_t(rect.x)+rect.width<=uint64_t(bounds.x)+bounds.width &&
        uint64_t(rect.y)+rect.height<=uint64_t(bounds.y)+bounds.height;
}
static_assert(RectContains({0,0,64,64},{16,8,48,56}));
static_assert(!RectContains({0,0,64,64},{16,8,49,56}));
static_assert(!RectContains({0,0,64,64},{-1,0,1,1}));
static_assert(!RectContains({0,0,64,64},{0,0,0,1}));
static_assert(!RectContains({0,0,UINT32_MAX,UINT32_MAX},{INT32_MAX,0,UINT32_MAX,1}));

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
    m_liveDeferred->ClearRecordingLookup();
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
    if(draw&&m_pipeline)for(const auto& binding:m_pipeline->m_vertexBuffers)
    {const auto found=m_vertexBuffers.find(binding.binding);if(found!=m_vertexBuffers.end())RecordLiveResourceLink(m_liveDeferred->FindObject(found->second.buffer),event);}
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
    if(terminal || m_recordingClosed)m_liveDeferred->ClearRecordingLookup();
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
        m_owner->RequireOwnerThread();
        FinishLiveRecording(m_recordingFailed?D::LiveOutcome::Failed:D::LiveOutcome::Discarded,true);
        m_owner->DiscardLivePrepared(this,D::LiveOutcome::Discarded);
        if(m_preparedNative) m_owner->DiscardCommandListNative(m_preparedNative);
        m_owner->m_recordedCommands.erase(this);
        m_references.clear();
        m_referenceHandles.clear();
        m_sharedReferences.reset();
        m_liveDeferred->ClearTrackedHandles();
    }
    DiscardLiveDeferred();
    D::RetireLiveObject(m_liveDeferred->m_livePosition.commandList);
}

bool ICommandList::CanRecordCommands()
{
    if(!m_owner || m_recordingClosed) m_recordingFailed = true;
    if(m_recordingFailed)m_liveDeferred->ClearRecordingLookup();
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

void ICommandList::CopyBuffer(BufferHandle source,uint32_t sourceOffset,
    BufferHandle destination,uint32_t destinationOffset,uint32_t size)
try
{
    if(!CanRecordCommands())return;
    if(m_rendering)
    {
        m_owner->ReportDiagnostic(IDevice::DiagnosticSeverity::Error,"CopyBuffer: must be recorded outside rendering.");
        m_recordingFailed=true;return;
    }
    if(!Track(source) || !Track(destination))return;
    if(source==destination || !CopyRangeFits(source->GetDesc().size,sourceOffset,size) ||
        !CopyRangeFits(destination->GetDesc().size,destinationOffset,size))
    {
        m_owner->ReportDiagnostic(IDevice::DiagnosticSeverity::Error,
            "CopyBuffer: requires distinct buffers, 4-byte aligned offsets/nonzero size and complete source/destination ranges.");
        m_recordingFailed=true;return;
    }
    RequireState(source,0,0,ResourceState::CopySource);
    RequireState(destination,0,0,ResourceState::CopyDestination);
    CopyBufferNative(source,sourceOffset,destination,destinationOffset,size);
}
catch(...) { m_recordingFailed=true; throw; }

void IDevice::CaptureRecordingLookup(ICommandList& commands)
{
    RequireOwnerThread();
    auto resources=CaptureRecordingResources();
    auto& borrowed=commands.m_liveDeferred->borrowedLookup;
    borrowed.clear();
    borrowed.reserve(m_borrowedTextures.size());
    for(const auto* handle:m_borrowedTextures)
    {
        const auto resource=m_resources.find(handle);
        if(resource!=m_resources.end())borrowed.push_back({handle,resource->second,m_imageGeneration});
    }
    commands.m_liveDeferred->resourceLookup=std::move(resources);
}

bool ICommandList::Track(const void* handle)
try
{
    if(!CanRecordCommands()) return false;
    auto& recent=m_liveDeferred->TrackedHandleSlot(handle);
    if(handle && recent==handle)return true;
    // Destroy* 전에 보유한 참조는 목록이 끝날 때까지 유지한다.
    if(m_referenceHandles.count(handle) || (m_sharedReferences && m_sharedReferences->handles.count(handle)))
    {recent=handle;return true;}
    std::shared_ptr<void> reference;
    const auto reject=[&] {
        m_owner->ReportDiagnostic(IDevice::DiagnosticSeverity::Error,
            "Command recording rejected a missing or retired resource; acquire/reset worker lists after preparing their shared resources.");
        m_recordingFailed=true;
        m_liveDeferred->ClearRecordingLookup();
        return false;
    };
    const auto pin=[&](const IDevice::RecordingResources* resources) {
        if(!resources)return false;
        const auto owned=resources->find(handle);
        if(owned==resources->end())return false;
        if(!owned->second->registered.load(std::memory_order_acquire))
        {m_recordingFailed=true;return true;}
        reference=owned->second->owner;
        return true;
    };
    bool found=false;
    if(IDevice::s_recordingDevice==m_owner && IDevice::s_recordingResources)
        found=pin(IDevice::s_recordingResources);
    if(!found)found=pin(m_liveDeferred->resourceLookup.get());
    if(m_recordingFailed)return reject();
    if(!found)
    {
        for(const auto& borrowed:m_liveDeferred->borrowedLookup)
        {
            if(borrowed.handle!=handle)continue;
            if(!borrowed.resource->registered.load(std::memory_order_acquire))
            {
                // The owner may start the next frame after Reset. A borrowed
                // wrapper can have the same address in the newly registered generation.
                if(m_owner->IsOwnerThread())break;
                return reject();
            }
            reference=borrowed.resource->owner;
            m_imageGeneration=borrowed.generation;
            found=true;
            break;
        }
    }
    if(!found)
    {
        // Only the owner can consult a changing registry. External workers use
        // the lookup captured at Acquire/Reset; internal workers prefer batch TLS.
        if(!m_owner->IsOwnerThread())return reject();
        const auto owned=m_owner->m_resources.find(handle);
        if(owned==m_owner->m_resources.end())return reject();
        reference=owned->second->owner;
        if(m_owner->m_borrowedTextures.count(handle)) m_imageGeneration=m_owner->m_imageGeneration;
    }
    if(!reference)return reject();
    // Only this list's recording thread grows its reference containers.
    m_references.push_back(std::move(reference));
    try { m_referenceHandles.insert(handle); }
    catch(...) { m_references.pop_back(); throw; }
    recent=handle;
    return true;
}
catch(...) { m_recordingFailed=true;m_liveDeferred->ClearRecordingLookup();throw; }

void ICommandList::ResourceBarrier(const ResourceBarrierDesc* barriers, uint32_t count)
try
{
    if(!CanRecordCommands()) return;
    if(count && !barriers) { m_recordingFailed = true; return; }
    if(count) {m_requiredResourceSets.clear();m_liveDeferred->ClearRecentStates();}
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
            auto& summary=m_closedStates.GetOrEmplace(key,barrier.before);
            if(!summary.Append(barrier.before,barrier.after,true))m_stateSummaryValid=false;
        }

    }
    ResourceBarrierNative(barriers, count);
}
catch(...) { m_recordingFailed=true; throw; }


void ICommandList::RequireState(const void* resource,uint32_t mip,uint32_t layer,ResourceState state)
try
{
    const uintptr_t object=reinterpret_cast<uintptr_t>(resource);
    auto& recent=*m_liveDeferred;
    uint32_t slot=static_cast<uint32_t>(recent.recentStates.size());
    for(uint32_t i=0;i<recent.recentStateCount;++i)
    {
        const auto& entry=recent.recentStates[i];
        if(entry.resource!=object || entry.mip!=mip || entry.layer!=layer) continue;
        if(entry.state==state) return;
        // Replace this key after the original path succeeds. Keeping another
        // old-state entry for it could incorrectly bypass a later requirement.
        slot=i;break;
    }
    const auto key=std::make_tuple(object,mip,layer);
    // 이 목록의 앞선 검사나 배리어가 같은 상태를 이미 보장하면 중복 검사만 합친다.
    auto& summary=m_closedStates.GetOrEmplace(key,state);
    if(!summary.Append(state,state,false))m_stateSummaryValid=false;
    if(slot==recent.recentStates.size())
    {
        if(recent.recentStateCount<recent.recentStates.size()) slot=recent.recentStateCount++;
        else
        {
            slot=recent.recentStateNext;
            recent.recentStateNext=(recent.recentStateNext+1)%recent.recentStates.size();
        }
    }
    recent.recentStates[slot]={object,mip,layer,state};
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
            const uint32_t channels=(format==Format::R16_UINT || format==Format::R16_FLOAT || format==Format::R32_UINT || format==Format::R32_FLOAT) ? 1u :
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
    Rect renderArea=desc.renderArea;
    if(!renderArea.width && !renderArea.height && !renderArea.x && !renderArea.y)
        renderArea={0,0,width,height};
    if(!RectContains({0,0,width,height},renderArea))
    {
        m_owner->ReportDiagnostic(IDevice::DiagnosticSeverity::Error,
            "BeginRendering: renderArea must be {} or a nonempty rectangle inside every attachment.");
        m_recordingFailed=true;return;
    }
    const bool partialArea=renderArea.x!=0 || renderArea.y!=0 || renderArea.width!=width || renderArea.height!=height;
    if(partialArea && !m_owner->Supports(Feature::RenderArea))
    {
        m_owner->ReportDiagnostic(IDevice::DiagnosticSeverity::Error,
            "BeginRendering: partial rendering requires Feature::RenderArea.");
        m_recordingFailed=true;return;
    }
    m_renderArea=partialArea?renderArea:Rect{};
    m_colorFormats.clear();
    for(uint32_t i=0;i<desc.colorAttachmentCount;++i)
        m_colorFormats.push_back(desc.colorAttachments[i].texture->GetDesc().format);
    m_depthStencilFormat=desc.depthStencilAttachment ? desc.depthStencilAttachment->texture->GetDesc().format : Format::Unknown;
    m_depthStencilState=desc.depthStencilAttachment?desc.depthStencilAttachment->state:ResourceState::Undefined;
    m_stencilAttachment=HasStencil(m_depthStencilFormat)&&desc.depthStencilAttachment->stencilLoadOp!=LoadOp::Undefined;
    m_stencilReference=false;
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
    BeginRenderingNative({colors.data(),desc.colorAttachmentCount,nativeDepth,renderArea});
}
catch(...) { m_recordingFailed=true; throw; }

void ICommandList::EndRendering() try
{ if(CanRecordCommands()) EndRenderingNative(); }
catch(...) { m_recordingFailed=true; throw; }
void ICommandList::BindGraphicsPipeline(PipelineHandle pipeline) try
{if(!Track(pipeline))return;if(pipeline->IsCompute()){m_recordingFailed=true;return;}m_liveDeferred->inlineConstants.Reset(pipeline->GetLayout().inlineConstantSize);m_resourceSet=nullptr;BindGraphicsPipelineNative(pipeline);}
catch(...) { m_recordingFailed=true; throw; }
void ICommandList::BindComputePipeline(PipelineHandle pipeline) try
{if(!Track(pipeline))return;if(m_rendering||!pipeline->IsCompute()){m_recordingFailed=true;return;}m_liveDeferred->inlineConstants.Reset(pipeline->GetLayout().inlineConstantSize);m_resourceSet=nullptr;BindComputePipelineNative(pipeline);}
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
        ValidateIndirectArguments(arguments,offset,sizeof(DispatchIndirectArguments),1,sizeof(DispatchIndirectArguments)))
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
    for(const auto& requirement:resources->GetStateRequirements())
    {
        if(requirement.buffer)RequireState(requirement.buffer,0,0,requirement.state);
        else for(uint32_t l=requirement.firstLayer;l<requirement.firstLayer+requirement.layerCount;++l)
            for(uint32_t m=requirement.firstMip;m<requirement.firstMip+requirement.mipCount;++m)
                RequireState(requirement.texture,m,l,requirement.state);
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
    const auto& layouts=m_pipeline->m_vertexBuffers;
    const auto layout=std::find_if(layouts.begin(),layouts.end(),[&](const VertexBufferLayout& value){return value.binding==binding;});
    if(layout==layouts.end() || layout->stride>buffer->GetDesc().size-offset)
    {m_recordingFailed=true;return;}
    RequireState(buffer,0,0,ResourceState::VertexBuffer);
    m_vertexBuffers[binding]={buffer,offset};
    BindVertexBufferNative(binding,buffer,offset);
}
catch(...) { m_recordingFailed=true; throw; }
void ICommandList::BindIndexBuffer(BufferHandle buffer,Format format,uint32_t offset)
try
{
    if(!Track(buffer))return;
    if(!m_rendering || (format!=Format::R16_UINT && format!=Format::R32_UINT) || offset>=buffer->GetDesc().size ||
        offset%FormatSize(format) || FormatSize(format)>buffer->GetDesc().size-offset || !HasUsage(buffer->GetDesc().usage,BufferUsage::Index))
    {m_recordingFailed=true;return;}
    RequireState(buffer,0,0,ResourceState::IndexBuffer);
    m_indexFormat=format;m_indexOffset=offset;
    BindIndexBufferNative(buffer,format,offset);
}
catch(...) { m_recordingFailed=true; throw; }
void ICommandList::SetInlineConstants(uint32_t offset, uint32_t size, const void* data)
try
{
    if(!CanRecordCommands()) return;
    if(!data || !size || !m_pipeline || (!m_pipeline->IsCompute() && !m_rendering) ||
        offset%4 || size%4 || offset>m_pipeline->GetLayout().inlineConstantSize ||
        size>m_pipeline->GetLayout().inlineConstantSize-offset) { m_recordingFailed = true; return; }
    m_liveDeferred->inlineConstants.Write(offset,size);
    SetInlineConstantsNative(offset, size, data);
}
catch(...) { m_recordingFailed=true; throw; }
void ICommandList::SetViewport(const Viewport& viewport) try
{
    if(!CanRecordCommands())return;
    if(!m_rendering || !std::isfinite(viewport.x+viewport.y+viewport.width+viewport.height+viewport.minDepth+viewport.maxDepth) ||
        viewport.width<=0 || viewport.height<=0 || viewport.minDepth<0 || viewport.maxDepth>1 || viewport.minDepth>viewport.maxDepth)
    {m_recordingFailed=true;return;}
    SetViewportNative(viewport);
}
catch(...) { m_recordingFailed=true; throw; }
void ICommandList::SetScissor(const Rect& rect) try
{
    if(!CanRecordCommands())return;
    if(!m_rendering || rect.x<0 || rect.y<0 || !rect.width || !rect.height ||
        (m_renderArea.width && !RectContains(m_renderArea,rect))) {m_recordingFailed=true;return;}
    SetScissorNative(rect);
}
catch(...) { m_recordingFailed=true; throw; }
void ICommandList::SetStencilReference(uint32_t reference)
try
{
    if(!CanRecordCommands())return;
    if(!m_rendering) {m_recordingFailed=true;return;}
    if(reference>255)
    {
        m_owner->ReportDiagnostic(IDevice::DiagnosticSeverity::Error,"SetStencilReference: the 8-bit stencil reference must be in [0,255]; high bits are not silently discarded.");
        m_recordingFailed=true;return;
    }
    m_stencilReference=true;
    SetStencilReferenceNative(reference);
}
catch(...) { m_recordingFailed=true; throw; }

bool ICommandList::ValidateBindings(bool indexed,bool compute,uint32_t count,uint32_t instances,uint32_t first,uint32_t firstInstance)
{
    const auto fail=[&](const char* message) {
        m_owner->ReportDiagnostic(IDevice::DiagnosticSeverity::Error,message);
        m_recordingFailed=true;
        return false;
    };
    if(!m_pipeline || m_pipeline->IsCompute()!=compute || m_pipeline->IsMesh())
        return fail("Draw/Dispatch: bind a pipeline of the matching type before execution.");
    if(!m_liveDeferred->inlineConstants.IsComplete())
        return fail("Draw/Dispatch: write all inline constants after selecting the pipeline.");
    if(!compute)
    {
        if(!m_rendering || !m_viewport || !m_scissor || m_colorFormats!=m_pipeline->m_colorFormats || m_depthStencilFormat!=m_pipeline->m_depthStencilFormat)
            return fail("Draw: rendering attachment formats must match the graphics pipeline.");
        if(m_pipeline->m_usesStencil&&(!m_stencilAttachment||!m_stencilReference))
            return fail("Draw: stencil pipelines require a configured stencil attachment and reference.");
        if(m_pipeline->m_requiresDepthWrite&&m_depthStencilState!=ResourceState::DepthWrite)
            return fail("Draw: depth or stencil writes require a DepthWrite attachment.");
        for(const auto& binding:m_pipeline->m_vertexBuffers)
        {
            const auto found=m_vertexBuffers.find(binding.binding);
            if(found==m_vertexBuffers.end())return fail("Draw: a vertex attribute binding has no vertex buffer.");
            const uint64_t firstElement=binding.stepMode==VertexStepMode::Instance?firstInstance:indexed?0:first;
            const uint64_t elements=binding.stepMode==VertexStepMode::Instance?instances:indexed?0:count;
            if(firstElement+elements>(found->second.buffer->GetDesc().size-found->second.offset)/binding.stride)
                return fail("Draw: vertex or instance range exceeds its bound buffer.");
            RequireState(found->second.buffer,0,0,ResourceState::VertexBuffer);
        }
        if(indexed)
        {
            if(!m_indexBuffer)return fail("DrawIndexed: no index buffer is bound.");
            if(uint64_t(first)+count>(m_indexBuffer->GetDesc().size-m_indexOffset)/FormatSize(m_indexFormat))
                return fail("DrawIndexed: index range exceeds its bound buffer.");
            RequireState(m_indexBuffer,0,0,ResourceState::IndexBuffer);
        }
    }
    if(m_resourceSet && m_resourceSet->GetPipeline()!=m_pipeline)
        return fail("Draw/Dispatch: bind a complete ResourceSet for the current pipeline after selecting that pipeline.");
    if(!m_resourceSet)
    {
        const auto& layout=m_pipeline->GetLayout();
        for(uint32_t i=0;i<layout.bindingCount;++i)
            if(layout.bindings[i].type!=ResourceBindingType::StaticSampler)
                return fail("Draw/Dispatch: bind a complete ResourceSet for the current pipeline after selecting that pipeline.");
    }
    return !m_resourceSet || RequireResourceSetStates(m_resourceSet);
}
bool ICommandList::ValidateIndirectArguments(BufferHandle arguments,uint32_t offset,uint32_t argumentSize,
    uint32_t drawCount,uint32_t stride)
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
    if(!HasUsage(desc.usage,BufferUsage::Indirect) ||
        !IndirectArgumentsFit(desc.size,offset,argumentSize,drawCount,stride))
    {
        m_owner->ReportDiagnostic(IDevice::DiagnosticSeverity::Error,
            "Indirect command: requires BufferUsage::Indirect, 4-byte aligned offset/stride, stride at least the argument size and every argument within the buffer.");
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
    if(!m_liveDeferred->inlineConstants.IsComplete())
        return fail("DispatchMesh: write all inline constants after selecting the pipeline.");
    if(!m_rendering || !m_viewport || !m_scissor || m_colorFormats!=m_pipeline->m_colorFormats || m_depthStencilFormat!=m_pipeline->m_depthStencilFormat)
        return fail("DispatchMesh: rendering attachment formats must match the mesh pipeline.");
    if(m_pipeline->m_usesStencil&&(!m_stencilAttachment||!m_stencilReference))
        return fail("DispatchMesh: stencil pipelines require a configured stencil attachment and reference.");
    if(m_pipeline->m_requiresDepthWrite&&m_depthStencilState!=ResourceState::DepthWrite)
        return fail("DispatchMesh: depth or stencil writes require a DepthWrite attachment.");
    if(m_resourceSet && m_resourceSet->GetPipeline()!=m_pipeline)
        return fail("DispatchMesh: bind a complete ResourceSet for the current pipeline after selecting that pipeline.");
    if(!m_resourceSet)
    {
        const auto& layout=m_pipeline->GetLayout();
        for(uint32_t i=0;i<layout.bindingCount;++i)
            if(layout.bindings[i].type!=ResourceBindingType::StaticSampler)
                return fail("DispatchMesh: bind a complete ResourceSet for the current pipeline after selecting that pipeline.");
    }
    return !m_resourceSet || RequireResourceSetStates(m_resourceSet);
}
void ICommandList::DrawInstanced(uint32_t vertices, uint32_t instances, uint32_t firstVertex, uint32_t firstInstance)
try
{ if(CanRecordCommands() && vertices && instances && ValidateBindings(false,false,vertices,instances,firstVertex,firstInstance)) DrawInstancedNative(vertices, instances, firstVertex, firstInstance); }
catch(...) { m_recordingFailed=true; throw; }
void ICommandList::DrawIndexedInstanced(uint32_t indices, uint32_t instances, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
try
{ if(CanRecordCommands() && indices && instances && ValidateBindings(true,false,indices,instances,firstIndex,firstInstance)) DrawIndexedInstancedNative(indices, instances, firstIndex, vertexOffset, firstInstance); }
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
        ValidateIndirectArguments(arguments,offset,sizeof(DrawIndirectArguments),1,sizeof(DrawIndirectArguments)))
        DrawIndirectNative(arguments,offset);
}
catch(...) { m_recordingFailed=true; throw; }
void ICommandList::DrawIndexedIndirect(BufferHandle arguments,uint32_t offset,uint32_t drawCount,uint32_t stride)
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
        ValidateIndirectArguments(arguments,offset,sizeof(DrawIndexedIndirectArguments),drawCount,stride) && drawCount)
        DrawIndexedIndirectNative(arguments,offset,drawCount,stride);
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
    // Recording maintains the summary; internal state mismatches remain submission errors.
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
    struct VertexBinding
    {
        BufferHandle buffer=nullptr;
        uint32_t offset=0;
    };
    std::array<VertexBinding,8> m_emittedVertexBindings={};
    BufferHandle m_emittedIndexBuffer=nullptr;
    Format m_emittedIndexFormat=Format::Unknown;
    uint32_t m_emittedIndexOffset=0;

    void InvalidateBufferBindings() noexcept
    {
        m_emittedVertexBindings={};
        m_emittedIndexBuffer=nullptr;
        m_emittedIndexFormat=Format::Unknown;
        m_emittedIndexOffset=0;
    }
    template<auto Method,class... Args>
    void RecordNative(Args... args)
    {
        static_assert((std::is_trivially_copyable_v<Args> && ...));
        static_assert(((sizeof(Args)%alignof(uint32_t)==0) && ...));
        const size_t position=m_commandBytes.size();
        if constexpr(sizeof...(Args)>0)
        {
            constexpr size_t argumentBytes=(size_t{0}+...+sizeof(Args));
            std::array<uint8_t,argumentBytes> packed;
            size_t cursor=0;
            const auto append=[&](const auto& value) {
                std::memcpy(packed.data()+cursor,&value,sizeof(value));cursor+=sizeof(value);
            };
            (append(args),...);
            m_commandBytes.insert(m_commandBytes.end(),packed.begin(),packed.end());
        }
        // Replay closures only hold an owner and an offset; draw/binding arguments share storage.
        m_commands.push_back([this,position](ICommandList& native) {
            size_t cursor=position;
            std::tuple<Args...> values{};
            std::apply([&](auto&... value) {
                const auto read=[&](auto& field) {
                    std::memcpy(&field,m_commandBytes.data()+cursor,sizeof(field));cursor+=sizeof(field);
                };
                (read(value),...);
            },values);
            std::apply([&](const auto&... value) {(native.*Method)(value...);},values);
            return true;
        });
    }
    void GlobalBarrierNative() override
    { m_commands.push_back([](ICommandList& native) { native.GlobalBarrierNative(); return true; });
      RecordLiveCommand(D::LiveCommandKind::Barrier,{},nullptr,0,0,1,D::LiveEventKind::Command,false,"Global barrier"); }
    void CopyBufferNative(BufferHandle source,uint32_t sourceOffset,BufferHandle destination,
        uint32_t destinationOffset,uint32_t size) override
    {
        RecordNative<&ICommandList::CopyBufferNative>(source,sourceOffset,destination,destinationOffset,size);
        RecordLiveCommand(D::LiveCommandKind::None,{sourceOffset,destinationOffset,size},destination,0,size,1,
            D::LiveEventKind::Command,false,"Copy buffer");
    }
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
        if(!count)return;
        std::vector<ResourceBarrierDesc> copy;
        copy.assign(values,values+count);
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
        // Every legal VB/IB bind follows BeginRendering, including after Reset
        // or cached-wrapper reuse; no private binding cache can cross that boundary.
        InvalidateBufferBindings();
        m_rendering=true;
        const auto pipeline=m_pipeline && !m_pipeline->IsCompute() ? m_pipeline : nullptr;
        m_pipeline=pipeline;
        std::vector<ColorAttachment> colors;
        if(desc.colorAttachmentCount) colors.assign(desc.colorAttachments,desc.colorAttachments+desc.colorAttachmentCount);
        const bool hasDepth=desc.depthStencilAttachment!=nullptr;
        const DepthStencilAttachment depth=hasDepth?*desc.depthStencilAttachment:DepthStencilAttachment{};
        const Rect renderArea=desc.renderArea;
        m_commands.push_back([colors=std::move(colors),depth,hasDepth,pipeline,renderArea](ICommandList& native) {
            native.BeginRenderingNative({colors.data(),static_cast<uint32_t>(colors.size()),hasDepth?&depth:nullptr,renderArea});
            if(pipeline)native.BindGraphicsPipelineNative(pipeline);
            return true;
        });
        RecordLiveCommand(D::LiveCommandKind::None,{desc.colorAttachmentCount,renderArea.x,renderArea.y,renderArea.width,renderArea.height},
            desc.colorAttachmentCount?static_cast<const void*>(desc.colorAttachments[0].texture):desc.depthStencilAttachment->texture,
            0,0,1,D::LiveEventKind::Command,false,"Begin rendering");
    }
    void EndRenderingNative() override
    {
        if(!m_rendering) { m_recordingFailed=true; return; }
        InvalidateBufferBindings();
        m_rendering=false;
        m_pipeline=nullptr;
        m_resourceSet=nullptr;
        m_vertexBuffers.clear();
        m_indexBuffer=nullptr;
        m_viewport=m_scissor=false;
        m_renderArea={};
        m_commands.push_back([](ICommandList& n){n.EndRenderingNative();return true;});
        RecordLiveCommand(D::LiveCommandKind::None,{},nullptr,0,0,1,D::LiveEventKind::Command,false,"End rendering");
    }
    void BindComputePipelineNative(PipelineHandle pipeline) override
    {
        InvalidateBufferBindings();
        m_pipeline=pipeline;
        RecordNative<&ICommandList::BindComputePipelineNative>(pipeline);
    }
    void ResetTimestampsNative(TimestampQueryHandle query,uint32_t first,uint32_t count) override
    {RecordNative<&ICommandList::ResetTimestampsNative>(query,first,count);
     RecordLiveCommand(D::LiveCommandKind::None,{first,count},query);}
    void WriteTimestampNative(TimestampQueryHandle query,uint32_t index) override
    {RecordNative<&ICommandList::WriteTimestampNative>(query,index);
     RecordLiveCommand(D::LiveCommandKind::None,{index},query);}
    void DispatchNative(uint32_t x,uint32_t y,uint32_t z) override
    {RecordNative<&ICommandList::DispatchNative>(x,y,z);
     RecordLiveCommand(D::LiveCommandKind::Dispatch,{x,y,z});}
    void DispatchIndirectNative(BufferHandle arguments,uint32_t offset) override
    {RecordNative<&ICommandList::DispatchIndirectNative>(arguments,offset);}
    void BindGraphicsPipelineNative(PipelineHandle pipeline) override
    {
        InvalidateBufferBindings();
        m_pipeline=pipeline;
        // A graph may select its pipeline before the callback opens rendering.
        if(m_rendering)RecordNative<&ICommandList::BindGraphicsPipelineNative>(pipeline);
        RecordLiveCommand(D::LiveCommandKind::Bind,{},pipeline,0,0,1,D::LiveEventKind::Command,false,"Graphics pipeline");
    }
    void BindResourceSetNative(ResourceSetHandle resources) override
    {
        RecordNative<&ICommandList::BindResourceSetNative>(resources);
#ifdef DY_LIVE_PROFILING_ENABLED
        // A no-op collector would still evaluate the per-binding object lookups.
        if(D::IsLiveCollectorEnabled())
        {
            RecordLiveCommand(D::LiveCommandKind::Bind,{},resources,resources->GetBindingCount(),0,1,D::LiveEventKind::Command,false,"Resource set");
            const auto resourceId=m_liveDeferred->FindObject(resources);
            for(uint32_t i=0;i<resources->GetBindingCount();++i)
            {
                const auto& binding=resources->GetBindings()[i];
                RecordLiveResourceLink(m_liveDeferred->FindObject(binding.buffer),resourceId);
                RecordLiveResourceLink(m_liveDeferred->FindObject(binding.texture),resourceId);
            }
        }
#endif
    }
    void BindVertexBufferNative(uint32_t binding, BufferHandle buffer, uint32_t offset) override
    {
        // Public validation, state requirements and ownership tracking already ran.
        // Only omit the repeated native opcode; keep the caller's Live Bind event.
        auto* cached=binding<m_emittedVertexBindings.size()?&m_emittedVertexBindings[binding]:nullptr;
        if(!cached || cached->buffer!=buffer || cached->offset!=offset)
        {
            RecordNative<&ICommandList::BindVertexBufferNative>(binding,buffer,offset);
            if(cached)*cached={buffer,offset};
        }
        RecordLiveCommand(D::LiveCommandKind::Bind,{binding,offset},buffer,0,0,1,D::LiveEventKind::Command,false,"Vertex buffer");
    }
    void BindIndexBufferNative(BufferHandle buffer, Format format, uint32_t offset) override
    {
        m_indexBuffer=buffer;
        if(m_emittedIndexBuffer!=buffer || m_emittedIndexFormat!=format || m_emittedIndexOffset!=offset)
        {
            RecordNative<&ICommandList::BindIndexBufferNative>(buffer,format,offset);
            m_emittedIndexBuffer=buffer;m_emittedIndexFormat=format;m_emittedIndexOffset=offset;
        }
        RecordLiveCommand(D::LiveCommandKind::Bind,{static_cast<int64_t>(format),offset},buffer,0,0,1,D::LiveEventKind::Command,false,"Index buffer");
    }
    void SetInlineConstantsNative(uint32_t offset,uint32_t size,const void* data) override
    {
        const uint32_t header[]={offset,size};
        const size_t position=m_commandBytes.size();
        if(m_commandBytes.max_size()-position<sizeof(header) ||
            m_commandBytes.max_size()-position-sizeof(header)<size)
            throw std::length_error("Recorded inline constants exceed the command-list storage limit.");
        const auto* headerBytes=reinterpret_cast<const uint8_t*>(header);
        const auto* bytes=static_cast<const uint8_t*>(data);
        m_commandBytes.insert(m_commandBytes.end(),headerBytes,headerBytes+sizeof(header));
        m_commandBytes.insert(m_commandBytes.end(),bytes,bytes+size);
        // Capture an offset rather than a data pointer: later records may grow the storage.
        m_commands.push_back([this,position](ICommandList& n) {
            uint32_t saved[2];
            std::memcpy(saved,m_commandBytes.data()+position,sizeof(saved));
            n.SetInlineConstantsNative(saved[0],saved[1],m_commandBytes.data()+position+sizeof(saved));
            return true;
        });
        RecordLiveCommand(D::LiveCommandKind::Bind,{offset,size},nullptr,0,size,1,D::LiveEventKind::Command,false,"Inline constants");
    }
    void SetViewportNative(const Viewport& viewport) override
    { m_viewport=true; RecordNative<&ICommandList::SetViewportNative>(viewport);
      RecordLiveCommand(D::LiveCommandKind::None,{},nullptr,0,0,1,D::LiveEventKind::Command,false,"Viewport"); }
    void SetScissorNative(const Rect& rect) override
    { m_scissor=true; RecordNative<&ICommandList::SetScissorNative>(rect);
      RecordLiveCommand(D::LiveCommandKind::None,{rect.x,rect.y,rect.width,rect.height},nullptr,0,0,1,D::LiveEventKind::Command,false,"Scissor"); }
    void SetStencilReferenceNative(uint32_t reference) override
    { RecordNative<&ICommandList::SetStencilReferenceNative>(reference);
      RecordLiveCommand(D::LiveCommandKind::Bind,{reference},nullptr,0,0,1,D::LiveEventKind::Command,false,"Stencil reference"); }
    void DrawInstancedNative(uint32_t vertices,uint32_t instances,uint32_t firstVertex,uint32_t firstInstance) override
    {
        if(!m_rendering || !m_pipeline || !m_viewport || !m_scissor) {m_recordingFailed=true;return;}
        RecordNative<&ICommandList::DrawInstancedNative>(vertices,instances,firstVertex,firstInstance);
        RecordLiveCommand(D::LiveCommandKind::Draw,{vertices,instances,firstVertex,firstInstance},nullptr,uint64_t(vertices)*instances);
    }
    void DrawIndexedInstancedNative(uint32_t indices,uint32_t instances,uint32_t firstIndex,int32_t vertexOffset,uint32_t firstInstance) override
    {
        if(!m_rendering || !m_pipeline || !m_indexBuffer || !m_viewport || !m_scissor) {m_recordingFailed=true;return;}
        RecordNative<&ICommandList::DrawIndexedInstancedNative>(indices,instances,firstIndex,vertexOffset,firstInstance);
        RecordLiveCommand(D::LiveCommandKind::DrawIndexed,{indices,instances,firstIndex,vertexOffset,firstInstance},nullptr,uint64_t(indices)*instances);
    }
    void DrawIndirectNative(BufferHandle arguments,uint32_t offset) override
    {
        if(!m_rendering || !m_pipeline || !m_viewport || !m_scissor) {m_recordingFailed=true;return;}
        RecordNative<&ICommandList::DrawIndirectNative>(arguments,offset);
    }
    void DrawIndexedIndirectNative(BufferHandle arguments,uint32_t offset,uint32_t drawCount,uint32_t stride) override
    {
        if(!m_rendering || !m_pipeline || !m_indexBuffer || !m_viewport || !m_scissor) {m_recordingFailed=true;return;}
        RecordNative<&ICommandList::DrawIndexedIndirectNative>(arguments,offset,drawCount,stride);
    }
    void DispatchMeshNative(uint32_t x,uint32_t y,uint32_t z) override
    {
        if(!m_rendering || !m_pipeline || !m_viewport || !m_scissor) {m_recordingFailed=true;return;}
        RecordNative<&ICommandList::DispatchMeshNative>(x,y,z);
        RecordLiveCommand(D::LiveCommandKind::DispatchMesh,{x,y,z});
    }
    bool CloseNative() override {return !m_rendering && !m_recordingFailed;}
};
ICommandList* ICommandList::CreateRecorded() {return new RecordedCommandList();}
}
