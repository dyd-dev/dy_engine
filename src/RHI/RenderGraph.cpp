#include "../Platform/Profiler/CollectorInternal.h"
#include <cstdio>
#include "dyf/RHI/RenderGraph.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/IDevice.h"
#include "Validation.h"
#include "../Platform/Profiler/RhiProfiler.h"

#include <atomic>

namespace dyf::RHI
{
    namespace
    {
        std::atomic<uint64_t> nextResourceId{1};

        bool Fail(const char* message)
        {
            std::fprintf(stderr, "[RenderGraph] dyf::RHI [error]: RenderGraph: %s\n", message);
            return false;
        }

        bool CanRead(ResourceState state)
        {
            return state == ResourceState::ShaderResource || state == ResourceState::VertexBuffer ||
                state == ResourceState::IndexBuffer || state == ResourceState::ConstantBuffer ||
                state == ResourceState::IndirectArgument ||
                state == ResourceState::DepthRead || state == ResourceState::UnorderedAccess;
        }

        bool CanWrite(ResourceState state)
        {
            return state == ResourceState::CopyDestination || state == ResourceState::RenderTarget ||
                state == ResourceState::DepthWrite || state == ResourceState::UnorderedAccess;
        }
    }

    RenderGraphPass& RenderGraphPass::Read(RGResourceHandle resource, ResourceState state)
    {
        m_reads.push_back({resource, state});
        ++m_revision;
        return *this;
    }

    RenderGraphPass& RenderGraphPass::Write(RGResourceHandle resource, ResourceState state)
    {
        m_writes.push_back({resource, state});
        ++m_revision;
        return *this;
    }

    RenderGraphPass& RenderGraphPass::SetPipeline(PipelineHandle pipeline)
    {
        m_pipeline = pipeline;
        ++m_revision;
        return *this;
    }

    RenderGraphPass& RenderGraphPass::SetExecute(std::function<void(ICommandList*)> callback)
    {
        m_executeCallback = std::move(callback);
        ++m_revision;
        return *this;
    }

    RenderGraphPass& RenderGraphPass::GlobalBarrier()
    {
        m_globalBarrier = true;
        ++m_revision;
        return *this;
    }

    bool RenderGraphPass::Execute(ICommandList* commands) const
    {
        if(!commands) return Fail("a command list is required.");
        if(!commands->CanRecordCommands() || commands->m_rendering || !m_executeCallback)
        {
            commands->m_recordingFailed = true;
            return Fail("pass requires an open command list outside rendering and a callback.");
        }
        if(m_pipeline)
        {
            if(!commands->Track(m_pipeline)) return Fail("pipeline is no longer owned by the command list's device.");
            if(m_pipeline->IsCompute()) commands->BindComputePipeline(m_pipeline);
            else commands->BindGraphicsPipeline(m_pipeline);
        }
        try { m_executeCallback(commands); }
        catch(...) { commands->m_recordingFailed = true; throw; }
        if(!commands->CanRecordCommands() || commands->m_rendering)
        {
            commands->m_recordingFailed = true;
            return Fail("callback failed recording, closed the command list, or left rendering open.");
        }
        return true;
    }

    RGResourceHandle RenderGraph::ImportTexture(const std::string& name, TextureHandle texture,
        ResourceState initialState, ResourceState finalState)
    {
        return Import(name, nullptr, texture, initialState, finalState);
    }

    RGResourceHandle RenderGraph::ImportBuffer(const std::string& name, BufferHandle buffer,
        ResourceState initialState, ResourceState finalState)
    {
        return Import(name, buffer, nullptr, initialState, finalState);
    }

    RGResourceHandle RenderGraph::Import(const std::string& name, BufferHandle buffer, TextureHandle texture,
        ResourceState initialState, ResourceState finalState)
    {
        const auto allowed = [&](ResourceState state) {
            return buffer ? IsBufferStateAllowed(buffer->GetDesc(), state) :
                texture && IsTextureStateAllowed(texture->GetDesc(), state);
        };
        if(name.empty() || (!buffer && !texture) || !allowed(initialState) ||
            finalState == ResourceState::Undefined || !allowed(finalState))
        {
            Fail("import requires a resource, a name and supported explicit boundary states.");
            return {};
        }
        const auto matches = [&](const Resource& resource) {
            return resource.buffer == buffer && resource.texture == texture &&
                resource.initialState == initialState && resource.finalState == finalState;
        };
        const auto named = m_resourceNameToHandle.find(name);
        if(named != m_resourceNameToHandle.end())
        {
            if(matches(m_resources[m_resourceIndices.at(named->second.id)])) return named->second;
            Fail("an existing import name refers to different resources or boundary states.");
            return {};
        }
        // 같은 자원의 다른 이름을 찾기 위해 매번 전체 목록을 순회하지 않는다.
        RGResourceHandle existing;
        if(buffer)
        {
            const auto found=m_bufferHandles.find(buffer);
            if(found!=m_bufferHandles.end())existing=found->second;
        }
        else
        {
            const auto found=m_textureHandles.find(texture);
            if(found!=m_textureHandles.end())existing=found->second;
        }
        if(existing.IsValid())
        {
            const auto& resource=m_resources[m_resourceIndices.at(existing.id)];
            if(!matches(resource))
            {
                Fail("aliases of one resource must use identical boundary states.");
                return {};
            }
            m_resourceNameToHandle.emplace(name, resource.handle);
            return resource.handle;
        }
        const RGResourceHandle handle{nextResourceId.fetch_add(1, std::memory_order_relaxed)};
        m_resourceIndices.emplace(handle.id, static_cast<uint32_t>(m_resources.size()));
        m_resources.push_back({buffer, texture, initialState, finalState, handle});
        m_resourceNameToHandle.emplace(name, handle);
        if(buffer)m_bufferHandles.emplace(buffer,handle);
        else m_textureHandles.emplace(texture,handle);
        m_compiled = false;
        return handle;
    }

    RenderGraphPass& RenderGraph::AddPass(const std::string& name, RGRecordingMode mode)
    {
        auto pass = std::make_unique<RenderGraphPass>(name, static_cast<uint32_t>(m_passes.size()), mode);
        auto& result = *pass;
        m_passes.push_back(std::move(pass));
        m_compiled = false;
        return result;
    }

    bool RenderGraph::Compile()
    {
        m_compiled = false;
        m_compiledRevisions.clear();
        m_executionOrder.clear();
        m_executionStages.clear();
        m_dependencies.clear();
        m_passBarriers.clear();
        m_finalBarriers.clear();
        const uint32_t passCount = static_cast<uint32_t>(m_passes.size());
        const uint32_t noPass = UINT32_MAX;
        std::vector<std::vector<uint32_t>> edges(passCount);
        std::vector<uint32_t> lastWriter(m_resources.size(), noPass);
        std::vector<std::vector<uint32_t>> readers(m_resources.size());
        std::vector<ResourceState> states;
        for(const auto& resource : m_resources) states.push_back(resource.initialState);
        m_passBarriers.resize(passCount);
        uint32_t lastGlobal = noPass;

        // 모든 import는 외부 내용을 가진다. 뒤쪽 writer가 앞선 read의 생산자라고 추측하지 않는다.
        for(uint32_t pass = 0; pass < passCount; ++pass)
        {
            if(!m_passes[pass]->HasExecuteCallback()) return Fail("every pass needs an execute callback.");
            if(lastGlobal != noPass) edges[lastGlobal].push_back(pass);
            if(m_passes[pass]->HasGlobalBarrier())
            {
                const uint32_t first = lastGlobal == noPass ? 0 : lastGlobal + 1;
                for(uint32_t previous = first; previous < pass; ++previous) edges[previous].push_back(pass);
                lastGlobal = pass;
            }
            struct Use { ResourceState state; bool write; };
            std::map<uint32_t, Use> uses;
            const auto collect = [&](const std::vector<RGResourceBinding>& bindings, bool write) {
                for(const auto& binding : bindings)
                {
                    const auto found = m_resourceIndices.find(binding.handle.id);
                    if(found == m_resourceIndices.end() ||
                        !(write ? CanWrite(binding.state) : CanRead(binding.state))) return false;
                    const auto& resource = m_resources[found->second];
                    if(!(resource.buffer ? IsBufferStateAllowed(resource.buffer->GetDesc(), binding.state) :
                        IsTextureStateAllowed(resource.texture->GetDesc(), binding.state))) return false;
                    auto& use = uses.emplace(found->second, Use{binding.state, write}).first->second;
                    if(use.state != binding.state) return false;
                    use.write |= write;
                }
                return true;
            };
            if(!collect(m_passes[pass]->GetReads(), false) || !collect(m_passes[pass]->GetWrites(), true))
                return Fail("invalid resource handle, read/write state, resource usage or conflicting states in one pass.");

            for(const auto& [index, use] : uses)
            {
                if(lastWriter[index] != noPass) edges[lastWriter[index]].push_back(pass);
                // Read/read의 상태가 달라도 layout/state 전환에는 실행 순서가 필요하다.
                if(use.write || states[index] != use.state)
                {
                    for(uint32_t reader : readers[index]) edges[reader].push_back(pass);
                    readers[index].clear();
                }
                if(use.write) lastWriter[index] = pass;
                else readers[index].push_back(pass);

                const auto& resource = m_resources[index];
                if(states[index] != use.state || CanWrite(use.state))
                    m_passBarriers[pass].push_back({resource.buffer, resource.texture, states[index], use.state, {}});
                states[index] = use.state;
            }
        }
        for(uint32_t index = 0; index < m_resources.size(); ++index)
        {
            const auto& resource = m_resources[index];
            if(states[index] != resource.finalState)
                m_finalBarriers.push_back({resource.buffer, resource.texture, states[index], resource.finalState, {}});
        }

        std::vector<uint32_t> stageIndices(passCount, 0);
        // Dependencies only point forward; registration order is also submission order.
        // Explicit backward dependencies would require a topological sort again.
        for(uint32_t pass = 0; pass < passCount; ++pass)
        {
            m_executionOrder.push_back(pass);
            const uint32_t stage = stageIndices[pass];
            if(m_executionStages.size() <= stage) m_executionStages.resize(size_t(stage) + 1);
            m_executionStages[stage].passIndices.push_back(pass);
            for(uint32_t next : edges[pass])
                stageIndices[next] = (std::max)(stageIndices[next], stage + 1);
        }
        for(uint32_t before=0;before<passCount;++before)
            for(uint32_t after:edges[before])m_dependencies.push_back({before,after});
        for(const auto& pass : m_passes) m_compiledRevisions.push_back(pass->GetRevision());
        m_compiled = true;
        return true;
    }

    bool RenderGraph::IsCompiled() const
    {
        if(!m_compiled || m_compiledRevisions.size() != m_passes.size()) return false;
        for(size_t index = 0; index < m_passes.size(); ++index)
            if(m_compiledRevisions[index] != m_passes[index]->GetRevision()) return false;
        return true;
    }

    bool RenderGraph::RequireResource(ICommandList* commands, const Resource& resource, ResourceState state) const
    {
        if(!commands->Track(resource.buffer ? static_cast<const void*>(resource.buffer) : resource.texture))
            return Fail("an imported resource is no longer owned by the command list's device.");
        if(resource.buffer) commands->RequireState(resource.buffer, 0, 0, state);
        else
        {
            const auto& desc = resource.texture->GetDesc();
            for(uint32_t layer = 0; layer < desc.depthOrArraySize; ++layer)
                for(uint32_t mip = 0; mip < desc.mipLevels; ++mip)
                    commands->RequireState(resource.texture, mip, layer, state);
        }
        return true;
    }

    bool RenderGraph::RecordBoundary(ICommandList* commands) const
    {
        if(m_timestampQuery)
        {
            if(!commands->Track(m_timestampQuery)) return Fail("timestamp query is no longer owned by the command list's device.");
            commands->ResetTimestamps(m_timestampQuery,0,2+2*static_cast<uint32_t>(m_passes.size()));
            commands->WriteTimestamp(m_timestampQuery,0);
        }
        // 모든 리소스를 먼저 검증/보유한다. 상태가 같아 배리어가 없어도 경계 계약은 검사한다.
        for(const auto& resource : m_resources)
            if(!RequireResource(commands, resource, resource.initialState)) return false;
        for(const auto& pass : m_passes)
            if(pass->GetPipeline() && !commands->Track(pass->GetPipeline()))
                return Fail("a pipeline is no longer owned by the command list's device.");
        return true;
    }

    bool RenderGraph::RecordPass(ICommandList* commands, uint32_t pass,uint64_t eventId) const
    {
        if(m_timestampQuery) commands->WriteTimestamp(m_timestampQuery,2+2*pass);
        if(m_passes[pass]->HasGlobalBarrier()) commands->GlobalBarrier();
        const auto& barriers = m_passBarriers[pass];
        commands->ResourceBarrier(barriers.data(), static_cast<uint32_t>(barriers.size()));
        if(!Platform::IsLiveCollectorEnabled())
        {
            if(!m_passes[pass]->Execute(commands))return false;
        }
        else
        {
            Platform::LiveEvent event;event.id=eventId;event.kind=Platform::LiveEventKind::CpuWork;
            event.stage=Platform::LiveStage::CpuPrepare;event.arguments={pass,
                static_cast<int64_t>(m_passes[pass]->GetReads().size()),static_cast<int64_t>(m_passes[pass]->GetWrites().size())};
            event.arguments[4]=LiveRhiThreadToken();
            const auto& name=m_passes[pass]->GetName();
            for(size_t i=0;eventId&&i<name.size()&&i+1<sizeof(event.label);++i)event.label[i]=name[i];
            event.beginNs=Platform::LiveNowNs();
            bool executed=false;
            try{executed=m_passes[pass]->Execute(commands);}
            catch(...){event.endNs=Platform::LiveNowNs();event.outcome=Platform::LiveOutcome::Failed;commands->RecordLiveInfo(event);throw;}
            event.endNs=Platform::LiveNowNs();event.outcome=executed?Platform::LiveOutcome::Observed:Platform::LiveOutcome::Failed;
            commands->RecordLiveInfo(event);
            if(!executed)return false;
            const auto linkResource=[&](const RGResourceBinding& binding,bool write)
            {
                const auto& resource=m_resources[m_resourceIndices.at(binding.handle.id)];
                const auto id=Platform::FindLiveObject(resource.buffer?static_cast<const void*>(resource.buffer):resource.texture);
                commands->RecordLiveResourceLink(write?eventId:id,write?id:eventId);
            };
            if(eventId)
            {
                for(const auto& binding:m_passes[pass]->GetReads())linkResource(binding,false);
                for(const auto& binding:m_passes[pass]->GetWrites())linkResource(binding,true);
            }
        }

        for(const auto& binding : m_passes[pass]->GetReads())
            if(!RequireResource(commands, m_resources[m_resourceIndices.at(binding.handle.id)], binding.state)) return false;
        for(const auto& binding : m_passes[pass]->GetWrites())
            if(!RequireResource(commands, m_resources[m_resourceIndices.at(binding.handle.id)], binding.state)) return false;
        if(m_timestampQuery) commands->WriteTimestamp(m_timestampQuery,3+2*pass);
        return commands->CanRecordCommands();
    }

    bool RenderGraph::Execute(ICommandList* commands) const
    {
        if(!commands) return Fail("a command list is required.");
        if(!IsCompiled() || !commands->CanRecordCommands() || commands->m_rendering)
        {
            commands->m_recordingFailed = true;
            return Fail("Compile must succeed before recording on an open command list outside rendering.");
        }
        if(!RecordBoundary(commands)) return false;
        const bool live=Platform::IsLiveDetailEnabled(commands->m_liveDeferred->m_liveContext.frame);
        std::vector<uint64_t> passEvents;
        if(live){passEvents.resize(m_passes.size());for(auto& id:passEvents)id=Platform::AllocateLiveId();}
        for(uint32_t pass : m_executionOrder)
            if(!RecordPass(commands,pass,live?passEvents[pass]:0))return false;
        if(live)for(const auto& dependency:m_dependencies)
            commands->RecordLiveResourceLink(passEvents[dependency.before],passEvents[dependency.after]);
        commands->ResourceBarrier(m_finalBarriers.data(), static_cast<uint32_t>(m_finalBarriers.size()));
        if(m_timestampQuery) commands->WriteTimestamp(m_timestampQuery,1);
        return commands->CanRecordCommands();
    }

    bool RenderGraph::ExecuteParallel(IDevice* device,
        std::vector<ICommandList*>& commandLists, uint32_t threadCount) const
    {
        if(!device || !commandLists.empty() || !IsCompiled())
            return Fail("parallel recording requires a device, an empty output and an unchanged compiled graph.");
        auto* liveState=device->GetLiveState();
        const auto caller=Platform::GetLiveContext();
        if(liveState&&caller.frame.device==liveState->device&&caller.frame.frame)
        {device->PrepareLiveControls(caller.frame);device->CaptureLiveGraphParallel(caller.frame);}
        // Boundary, passes and an empty final list belong to this actual recording operation.
        Platform::LiveFrameScope liveFrame(liveState?liveState->device:0,liveState?liveState->recordingSource:0);
        liveFrame.SetOutcome(Platform::LiveOutcome::Failed);
        if(liveState)device->PrepareLiveControls(liveFrame.GetKey());
        const auto policyBegin=liveState?Platform::LiveNowNs():0;
        const bool requestedParallel=device->CaptureLiveGraphParallel(liveFrame.GetKey());
        const uint32_t effectiveThreads=requestedParallel?threadCount:1;
        if(liveState)
        {
            Platform::LiveEvent policy;policy.context=Platform::GetLiveContext();
            policy.kind=Platform::LiveEventKind::CpuWork;policy.stage=Platform::LiveStage::CpuPrepare;
            policy.beginNs=policyBegin;policy.endNs=Platform::LiveNowNs();
            const auto workers=effectiveThreads?effectiveThreads:(std::max)(1u,std::thread::hardware_concurrency());
            policy.arguments={requestedParallel,workers>1,workers,!requestedParallel?1:workers<=1?3:0,LiveRhiThreadToken()};
            const char label[]="RenderGraph recording policy";std::copy(std::begin(label),std::end(label),policy.label);
            Platform::RecordLiveEvent(policy);
        }
        struct Recording
        {
            IDevice& device;
            std::vector<ICommandList*> lists;
            ~Recording() { for(auto* list : lists) device.DestroyCommandList(list); }
            ICommandList* Acquire()
            {
                auto* list = device.AcquireCommandList();
                try { lists.push_back(list); }
                catch(...) { device.DestroyCommandList(list); throw; }
                return list;
            }
        } recording{*device, {}};
        // Keep the existing GPU order: stage scheduling only changes CPU recording.
        // Grouping submissions by stage could move a reader before its state transition.
        recording.lists.reserve(m_passes.size() + 2);
        auto* boundary = recording.Acquire();
        if(!boundary || !RecordBoundary(boundary) || !boundary->Close()) return false;
        auto references=std::make_shared<ICommandList::ReferenceSnapshot>();
        references->resources.swap(boundary->m_references);
        references->handles.swap(boundary->m_referenceHandles);
        boundary->m_sharedReferences=references;
        std::vector<ICommandList*> passes(m_passes.size(), nullptr);
        for(uint32_t pass : m_executionOrder)
        {
            if(!(passes[pass] = recording.Acquire())) return false;
            passes[pass]->m_sharedReferences = references;
            passes[pass]->m_imageGeneration = boundary->m_imageGeneration;
        }
        auto* final = recording.Acquire();
        if(!final) return false;
        final->m_sharedReferences = references;
        final->m_imageGeneration = boundary->m_imageGeneration;
        std::vector<uint8_t> recorded(m_passes.size(), 0);
        const bool live=Platform::IsLiveDetailEnabled();
        std::vector<uint64_t> passEvents;
        if(live){passEvents.resize(m_passes.size());for(auto& id:passEvents)id=Platform::AllocateLiveId();}
        const auto context=Platform::GetLiveContext();
        const auto record = [&](uint32_t pass) {
            Platform::LiveContextScope restore(context);
            recorded[pass]=RecordPass(passes[pass],pass,live?passEvents[pass]:0)&&passes[pass]->Close();
        };
        std::vector<uint32_t> independent;
        independent.reserve(m_executionOrder.size());
        for(uint32_t pass : m_executionOrder)
            if(m_passes[pass]->GetRecordingMode()==RGRecordingMode::Independent) independent.push_back(pass);
        device->RecordParallel(static_cast<uint32_t>(independent.size()), effectiveThreads,
            [&](uint32_t index) { record(independent[index]); });
        for(uint32_t pass : independent) if(!recorded[pass]) return false;
        std::vector<uint32_t> ordered;
        for(const auto& stage : m_executionStages)
        {
            ordered.clear();
            for(uint32_t pass : stage.passIndices)
                if(m_passes[pass]->GetRecordingMode()!=RGRecordingMode::Independent) ordered.push_back(pass);
            device->RecordParallel(static_cast<uint32_t>(ordered.size()), effectiveThreads,
                [&](uint32_t index) { record(ordered[index]); });
            for(uint32_t pass : ordered) if(!recorded[pass]) return false;
        }
        final->ResourceBarrier(m_finalBarriers.data(), static_cast<uint32_t>(m_finalBarriers.size()));
        if(m_timestampQuery) final->WriteTimestamp(m_timestampQuery,1);
        if(!final->Close()) return false;
        if(!device->PrepareCommandLists(recording.lists.data(),
            static_cast<uint32_t>(recording.lists.size()), effectiveThreads)) return false;
        if(live)for(const auto& dependency:m_dependencies)
            passes[dependency.after]->RecordLiveResourceLink(passEvents[dependency.before],passEvents[dependency.after]);
        commandLists.swap(recording.lists);
        liveFrame.SetOutcome(Platform::LiveOutcome::Observed);
        return true;
    }

    void RenderGraph::Reset()
    {
        m_resources.clear();
        m_resourceNameToHandle.clear();
        m_resourceIndices.clear();
        m_bufferHandles.clear();
        m_textureHandles.clear();
        m_passes.clear();
        m_compiledRevisions.clear();
        m_executionOrder.clear();
        m_executionStages.clear();
        m_dependencies.clear();
        m_passBarriers.clear();
        m_finalBarriers.clear();
        m_compiled = false;
        m_timestampQuery=nullptr;
    }

    std::vector<std::string> RenderGraph::GetExecutionOrderNames() const
    {
        std::vector<std::string> names;
        if(IsCompiled())
            for(uint32_t index : m_executionOrder) names.push_back(m_passes[index]->GetName());
        return names;
    }

    const RenderGraphPass* RenderGraph::GetPass(uint32_t index) const
    {
        return index < m_passes.size() ? m_passes[index].get() : nullptr;
    }
}
