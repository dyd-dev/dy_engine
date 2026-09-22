#include "dyf/Platform/Log.h"
#include "dyf/RHI/RenderGraph.h"
#include "dyf/RHI/ICommandList.h"
#include "RHI/Validation.h"

#include <atomic>
#include <cstdio>
#include <queue>

namespace dyf::RHI
{
    namespace
    {
        std::atomic<uint64_t> nextResourceId{1};

        bool Fail(const char* message)
        {
            dyf::Platform::Log::Writef(dyf::Platform::LogLevel::Error, "RenderGraph", __FILE__, __LINE__, "dyf::RHI [error]: RenderGraph: %s", message);
            return false;
        }

        bool CanRead(ResourceState state)
        {
            return state == ResourceState::ShaderResource || state == ResourceState::VertexBuffer ||
                state == ResourceState::IndexBuffer || state == ResourceState::ConstantBuffer ||
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
        for(const auto& resource : m_resources)
        {
            if(resource.buffer != buffer || resource.texture != texture) continue;
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
        m_compiled = false;
        return handle;
    }

    RenderGraphPass& RenderGraph::AddPass(const std::string& name)
    {
        auto pass = std::make_unique<RenderGraphPass>(name, static_cast<uint32_t>(m_passes.size()));
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

        // 모든 import는 외부 내용을 가진다. 뒤쪽 writer가 앞선 read의 생산자라고 추측하지 않는다.
        for(uint32_t pass = 0; pass < passCount; ++pass)
        {
            if(!m_passes[pass]->HasExecuteCallback()) return Fail("every pass needs an execute callback.");
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
                    const auto inserted = uses.emplace(found->second, Use{binding.state, write});
                    if(!inserted.second)
                    {
                        if(inserted.first->second.state != binding.state) return false;
                        inserted.first->second.write |= write;
                    }
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

        std::vector<uint32_t> inDegree(passCount, 0);
        for(auto& successors : edges)
        {
            std::sort(successors.begin(), successors.end());
            successors.erase(std::unique(successors.begin(), successors.end()), successors.end());
            for(uint32_t successor : successors) ++inDegree[successor];
        }
        std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<uint32_t>> ready;
        for(uint32_t pass = 0; pass < passCount; ++pass) if(!inDegree[pass]) ready.push(pass);
        while(!ready.empty())
        {
            const uint32_t pass = ready.top();
            ready.pop();
            m_executionOrder.push_back(pass);
            for(uint32_t next : edges[pass]) if(--inDegree[next] == 0) ready.push(next);
        }
        if(m_executionOrder.size() != passCount) return Fail("resource dependencies contain a cycle.");
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

    bool RenderGraph::Execute(ICommandList* commands) const
    {
        if(!commands) return Fail("a command list is required.");
        if(!IsCompiled() || !commands->CanRecordCommands() || commands->m_rendering)
        {
            commands->m_recordingFailed = true;
            return Fail("Compile must succeed before recording on an open command list outside rendering.");
        }
        const auto require = [&](const Resource& resource, ResourceState state) {
            if(resource.buffer) commands->RequireState(resource.buffer, 0, 0, state);
            else
            {
                const auto& desc = resource.texture->GetDesc();
                for(uint32_t layer = 0; layer < desc.depthOrArraySize; ++layer)
                    for(uint32_t mip = 0; mip < desc.mipLevels; ++mip)
                        commands->RequireState(resource.texture, mip, layer, state);
            }
        };
        // 모든 리소스를 먼저 검증/보유한다. 상태가 같아 배리어가 없어도 경계 계약은 검사한다.
        for(const auto& resource : m_resources)
        {
            if(!commands->Track(resource.buffer ? static_cast<const void*>(resource.buffer) : resource.texture))
                return Fail("an imported resource is no longer owned by the command list's device.");
            require(resource, resource.initialState);
        }
        for(const auto& pass : m_passes)
            if(pass->GetPipeline() && !commands->Track(pass->GetPipeline()))
                return Fail("a pipeline is no longer owned by the command list's device.");
        for(uint32_t pass : m_executionOrder)
        {
            const auto& barriers = m_passBarriers[pass];
            commands->ResourceBarrier(barriers.data(), static_cast<uint32_t>(barriers.size()));
            if(!m_passes[pass]->Execute(commands)) return false;
            for(const auto& binding : m_passes[pass]->GetReads())
                require(m_resources[m_resourceIndices.at(binding.handle.id)], binding.state);
            for(const auto& binding : m_passes[pass]->GetWrites())
                require(m_resources[m_resourceIndices.at(binding.handle.id)], binding.state);
        }
        commands->ResourceBarrier(m_finalBarriers.data(), static_cast<uint32_t>(m_finalBarriers.size()));
        return commands->CanRecordCommands();
    }

    void RenderGraph::Reset()
    {
        m_resources.clear();
        m_resourceNameToHandle.clear();
        m_resourceIndices.clear();
        m_passes.clear();
        m_compiledRevisions.clear();
        m_executionOrder.clear();
        m_passBarriers.clear();
        m_finalBarriers.clear();
        m_compiled = false;
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
