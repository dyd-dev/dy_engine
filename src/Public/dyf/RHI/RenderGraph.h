#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "Barrier.h"

namespace dyf::RHI
{
    class ICommandList;

    struct RGResourceHandle
    {
        uint64_t id = 0;
        [[nodiscard]] bool IsValid() const { return id != 0; }
        bool operator==(const RGResourceHandle& other) const { return id == other.id; }
        bool operator!=(const RGResourceHandle& other) const { return id != other.id; }
    };

    struct RGResourceBinding
    {
        RGResourceHandle handle;
        ResourceState state = ResourceState::Undefined;
    };

    class RenderGraphPass
    {
    public:
        explicit RenderGraphPass(std::string name, uint32_t index)
            : m_name(std::move(name)), m_index(index) {}
        RenderGraphPass(const RenderGraphPass&) = delete;
        RenderGraphPass& operator=(const RenderGraphPass&) = delete;
        RenderGraphPass(RenderGraphPass&&) = delete;
        RenderGraphPass& operator=(RenderGraphPass&&) = delete;

        // 같은 리소스의 Read/Write는 등록 순서의 직전 내용을 사용한다.
        // 한 패스의 동일 리소스는 한 상태만 사용한다. UAV 읽기/쓰기는 둘 다 선언한다.
        RenderGraphPass& Read(RGResourceHandle resource, ResourceState state);
        RenderGraphPass& Write(RGResourceHandle resource, ResourceState state);
        RenderGraphPass& SetPipeline(PipelineHandle pipeline);
        // callback은 rendering을 직접 시작/종료한다. graphics 상수/바인딩은 rendering 안에서 설정한다.
        // 선언한 리소스의 상태와 실행 중인 그래프는 callback에서 바꾸지 않는다.
        RenderGraphPass& SetExecute(std::function<void(ICommandList*)> callback);

        [[nodiscard]] const std::string& GetName() const { return m_name; }
        [[nodiscard]] uint32_t GetIndex() const { return m_index; }
        [[nodiscard]] uint64_t GetRevision() const { return m_revision; }
        [[nodiscard]] PipelineHandle GetPipeline() const { return m_pipeline; }
        [[nodiscard]] const std::vector<RGResourceBinding>& GetReads() const { return m_reads; }
        [[nodiscard]] const std::vector<RGResourceBinding>& GetWrites() const { return m_writes; }
        [[nodiscard]] bool HasExecuteCallback() const { return static_cast<bool>(m_executeCallback); }
        [[nodiscard]] bool Execute(ICommandList* commandList) const;

    private:
        std::string m_name;
        uint32_t m_index = 0;
        uint64_t m_revision = 0;
        PipelineHandle m_pipeline = nullptr;
        std::vector<RGResourceBinding> m_reads;
        std::vector<RGResourceBinding> m_writes;
        std::function<void(ICommandList*)> m_executeCallback;
    };

    class RenderGraph
    {
    public:
        RenderGraph() = default;
        ~RenderGraph() = default;
        RenderGraph(const RenderGraph&) = delete;
        RenderGraph& operator=(const RenderGraph&) = delete;
        RenderGraph(RenderGraph&&) = default;
        RenderGraph& operator=(RenderGraph&&) = default;

        // 시작/종료 상태는 그래프 실행 경계의 실제 상태다. finalState=Undefined는 허용하지 않는다.
        // 리소스 전체(모든 mip/layer)가 같은 경계 상태여야 하며 기록까지 호출자가 수명을 유지한다.
        // 같은 리소스의 별칭은 같은 핸들이다. 이름/상태가 충돌하거나 null이면 빈 핸들을 반환한다.
        [[nodiscard]] RGResourceHandle ImportTexture(const std::string& name, TextureHandle texture,
            ResourceState initialState, ResourceState finalState);
        [[nodiscard]] RGResourceHandle ImportBuffer(const std::string& name, BufferHandle buffer,
            ResourceState initialState, ResourceState finalState);
        RenderGraphPass& AddPass(const std::string& name);

        // 등록 순서의 RAW/WAR/WAW 의존성과 패스별/종료 배리어를 계산한다.
        [[nodiscard]] bool Compile();
        // Compile 이후 수정하지 않은 계획만 기록한다. 성공은 GPU 제출 성공을 뜻하지 않는다.
        // 실패한 commandList는 Reset/Destroy한다. callback 예외는 기록 실패 표시 후 전파한다.
        // Execute는 그래프 상태를 갱신하지 않는다. 재실행 시 initialState를 다시 만족해야 한다.
        // 현재 단일 스레드 기록용이다. 구성/Compile/Reset과 Execute를 동시에 호출하지 않는다.
        [[nodiscard]] bool Execute(ICommandList* commandList) const;
        void Reset();

        [[nodiscard]] bool IsCompiled() const;
        [[nodiscard]] const std::vector<uint32_t>& GetExecutionOrderIndices() const { return m_executionOrder; }
        [[nodiscard]] std::vector<std::string> GetExecutionOrderNames() const;
        [[nodiscard]] const RenderGraphPass* GetPass(uint32_t index) const;

    private:
        struct Resource
        {
            BufferHandle buffer = nullptr;
            TextureHandle texture = nullptr;
            ResourceState initialState = ResourceState::Undefined;
            ResourceState finalState = ResourceState::Undefined;
            RGResourceHandle handle;
        };
        RGResourceHandle Import(const std::string& name, BufferHandle buffer, TextureHandle texture,
            ResourceState initialState, ResourceState finalState);
        std::vector<Resource> m_resources;
        std::unordered_map<std::string, RGResourceHandle> m_resourceNameToHandle;
        std::unordered_map<uint64_t, uint32_t> m_resourceIndices;
        std::vector<std::unique_ptr<RenderGraphPass>> m_passes;
        std::vector<uint32_t> m_executionOrder;
        std::vector<std::vector<ResourceBarrierDesc>> m_passBarriers;
        std::vector<ResourceBarrierDesc> m_finalBarriers;
        std::vector<uint64_t> m_compiledRevisions;
        bool m_compiled = false;
    };
}
