#pragma once

#include <cstddef>
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
    class IDevice;

    // Independent는 다른 패스 callback과 변경 가능한 CPU 상태를 공유하지 않는다.
    // 기록 모드는 GPU 실행 순서나 배리어를 바꾸지 않는다.
    enum class RGRecordingMode : uint8_t { Ordered, Independent };

    struct RGExecutionStage
    {
        std::vector<uint32_t> passIndices;
    };
    struct RGDependency {uint32_t before=0,after=0;};

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
        explicit RenderGraphPass(std::string name, uint32_t index,
            RGRecordingMode mode = RGRecordingMode::Ordered)
            : m_name(std::move(name)), m_index(index), m_recordingMode(mode) {}
        RenderGraphPass(const RenderGraphPass&) = delete;
        RenderGraphPass& operator=(const RenderGraphPass&) = delete;
        RenderGraphPass(RenderGraphPass&&) = delete;
        RenderGraphPass& operator=(RenderGraphPass&&) = delete;

        // 같은 리소스의 Read/Write는 등록 순서의 직전 내용을 사용한다.
        // 한 패스의 동일 리소스는 한 상태만 사용한다. UAV 읽기/쓰기는 둘 다 선언한다.
        RenderGraphPass& Read(RGResourceHandle resource, ResourceState state);
        RenderGraphPass& Write(RGResourceHandle resource, ResourceState state);
        RenderGraphPass& SetPipeline(PipelineHandle pipeline);
        // All earlier passes precede this pass and all later passes follow it.
        // Emits a queue-wide memory barrier, including resources outside this graph.
        // Resource states/layouts are unchanged; declare graph uses for state transitions.
        RenderGraphPass& GlobalBarrier();
        // callback은 rendering을 직접 시작/종료한다. graphics 상수/바인딩은 rendering 안에서 설정한다.
        // 선언한 리소스의 상태와 실행 중인 그래프는 callback에서 바꾸지 않는다.
        RenderGraphPass& SetExecute(std::function<void(ICommandList*)> callback);

        [[nodiscard]] const std::string& GetName() const { return m_name; }
        [[nodiscard]] uint32_t GetIndex() const { return m_index; }
        [[nodiscard]] RGRecordingMode GetRecordingMode() const { return m_recordingMode; }
        [[nodiscard]] uint64_t GetRevision() const { return m_revision; }
        [[nodiscard]] PipelineHandle GetPipeline() const { return m_pipeline; }
        [[nodiscard]] const std::vector<RGResourceBinding>& GetReads() const { return m_reads; }
        [[nodiscard]] const std::vector<RGResourceBinding>& GetWrites() const { return m_writes; }
        [[nodiscard]] bool HasExecuteCallback() const { return static_cast<bool>(m_executeCallback); }
        [[nodiscard]] bool HasGlobalBarrier() const { return m_globalBarrier; }
        [[nodiscard]] bool Execute(ICommandList* commandList) const;

    private:
        std::string m_name;
        uint32_t m_index = 0;
        RGRecordingMode m_recordingMode = RGRecordingMode::Ordered;
        uint64_t m_revision = 0;
        PipelineHandle m_pipeline = nullptr;
        std::vector<RGResourceBinding> m_reads;
        std::vector<RGResourceBinding> m_writes;
        std::function<void(ICommandList*)> m_executeCallback;
        bool m_globalBarrier = false;
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
        RenderGraphPass& AddPass(const std::string& name, RGRecordingMode mode = RGRecordingMode::Ordered);

        // 등록 순서의 RAW/WAR/WAW 의존성과 패스별/종료 배리어를 계산한다.
        [[nodiscard]] bool Compile();
        // Compile 이후 수정하지 않은 계획만 기록한다. 성공은 GPU 제출 성공을 뜻하지 않는다.
        // 실패한 commandList는 Reset/Destroy한다. callback 예외는 기록 실패 표시 후 전파한다.
        // Execute는 그래프 상태를 갱신하지 않는다. 재실행 시 initialState를 다시 만족해야 한다.
        // 구성/Compile/Reset과 실행을 동시에 호출하지 않는다.
        [[nodiscard]] bool Execute(ICommandList* commandList) const;
        // Independent 패스는 GPU 의존성과 무관하게 한 묶음으로 병렬 기록한다.
        // Ordered 패스는 기존 의존 단계 순서대로 기록하며 같은 단계 안에서는 병렬 기록한다.
        // 각 목록의 네이티브 기록도 병렬화하며 GPU 제출 순서는 그대로 유지한다.
        // 성공 시 비어 있던 commandLists에 닫힌 목록을 제출 순서대로 반환한다.
        // 호출자는 목록을 순서대로 한 번에 Submit하고 DestroyCommandList로 해제한다.
        // GPU 완료까지의 자원 보유와 완료 펜스는 기존 IDevice::Submit 계약을 따른다.
        // 실패 시 출력은 비어 있으며 모든 작업 합류/목록 폐기 후 callback 예외를 전파한다.
        // 작업자는 장치가 관리한다. threadCount=0은 하드웨어 작업자 수를 선택하고,
        // 1이면 현재 스레드에서 기록한다. 같은 작업자에서 재진입해도 직렬 기록한다.
        // callback의 공유 데이터는 호출자가 동기화한다. callback에서 Submit/Present,
        // 목록 Close/Reset/Destroy 또는 그래프를 변경하지 않는다. 첫 callback 전에
        // 모든 import/pipeline을 보유하며 소유 핸들 해제 후에도 기록된 참조는 유지한다.
        // 반환 전에 네이티브 기록까지 완료한다. GPU 제출은 호출자가 수행한다.
        [[nodiscard]] bool ExecuteParallel(IDevice* device,
            std::vector<ICommandList*>& commandLists, uint32_t threadCount = 0) const;
        // Optional caller-owned query with at least 2 + 2 * pass-count slots.
        // Slots 0/1 span the graph; slots 2+2*pass / 3+2*pass surround that pass,
        // including its barriers. Reset is recorded once before all pass lists.
        // The caller retains the query through Submit and reads after its completion fence.
        void SetTimestampQuery(TimestampQueryHandle query) { m_timestampQuery=query; }
        void Reset();

        [[nodiscard]] bool IsCompiled() const;
        [[nodiscard]] const std::vector<uint32_t>& GetExecutionOrderIndices() const { return m_executionOrder; }
        [[nodiscard]] const std::vector<RGExecutionStage>& GetExecutionStages() const { return m_executionStages; }
        [[nodiscard]] const std::vector<RGDependency>& GetDependencies() const { return m_dependencies; }
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
        struct ResourceUse
        {
            uint32_t index;
            ResourceState state;
        };
        RGResourceHandle Import(const std::string& name, BufferHandle buffer, TextureHandle texture,
            ResourceState initialState, ResourceState finalState);
        bool RequireResource(ICommandList* commands, const Resource& resource, ResourceState state) const;
        bool RecordBoundary(ICommandList* commands) const;
        bool RecordPass(ICommandList* commands, uint32_t pass,uint64_t eventId=0) const;
        std::vector<Resource> m_resources;
        std::unordered_map<std::string, RGResourceHandle> m_resourceNameToHandle;
        std::unordered_map<uint64_t, uint32_t> m_resourceIndices;
        std::unordered_map<BufferHandle, RGResourceHandle> m_bufferHandles;
        std::unordered_map<TextureHandle, RGResourceHandle> m_textureHandles;
        std::vector<std::unique_ptr<RenderGraphPass>> m_passes;
        std::vector<uint32_t> m_executionOrder;
        std::vector<RGExecutionStage> m_executionStages;
        std::vector<RGDependency> m_dependencies;
        std::vector<std::vector<ResourceBarrierDesc>> m_passBarriers;
        std::vector<ResourceBarrierDesc> m_finalBarriers;
        std::vector<ResourceUse> m_passUses;
        std::vector<std::size_t> m_passUseOffsets;
        std::vector<uint64_t> m_compiledRevisions;
        bool m_compiled = false;
        TimestampQueryHandle m_timestampQuery = nullptr;
    };
}
