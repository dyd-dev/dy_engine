#pragma once

#include <array>
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include "dyf/Renderer.h"

namespace dyf
{
class ModelScene;
struct SkinInfluence;

enum class SkinningExecutionMode : uint8_t
{
    VertexShader,
    ComputePreSkin
};

// 모델 변형에 사용하는 단계만 교체한다. 다른 렌더링 단계는 Renderer의 설정을 사용한다.
struct ModelShaderDesc
{
    RHI::ShaderDesc vertex;
    RHI::ShaderDesc shadowVertex;
    RHI::ShaderDesc skinningCompute;
};
struct ModelOptimizationConfig
{
    bool reuseFrameBuffers=true;
    bool skipUnchangedUploads=true;
    bool cacheStaticMeshes=true;
    bool cacheInfluences=true;
};

// 모델 변형과 그 GPU 자원은 선택 확장이 소유한다. renderer는 이 객체보다 오래 살아 있어야 한다.
// 각 Render가 끝나면 기존 정점 설정을 복원하여 같은 Renderer로 일반 Scene도 그릴 수 있다.
class ModelRenderer
{
public:
    explicit ModelRenderer(Renderer& renderer);
    ~ModelRenderer();
    ModelRenderer(const ModelRenderer&) = delete;
    ModelRenderer& operator=(const ModelRenderer&) = delete;

    [[nodiscard]] bool Render(const ModelScene&, Image* readback = nullptr);
    [[nodiscard]] bool Render(const ModelScene&, const Camera&, Image* readback = nullptr);
    [[nodiscard]] bool Render(const ModelScene&, const Canvas& overlay, Image* readback = nullptr);
    [[nodiscard]] bool Render(const ModelScene&, const Camera&, const Canvas& overlay, Image* readback = nullptr);

    [[nodiscard]] SkinningExecutionMode GetSkinningExecutionMode() const { return m_mode; }
    bool SetSkinningExecutionMode(SkinningExecutionMode);
    [[nodiscard]] ModelOptimizationConfig GetOptimizationConfig() const noexcept {return m_pendingOptimizationConfig?*m_pendingOptimizationConfig:m_optimizationConfig;}
    [[nodiscard]] ModelOptimizationConfig GetAppliedOptimizationConfig() const noexcept {return m_optimizationConfig;}
    bool SetOptimizationConfig(const ModelOptimizationConfig& value) noexcept {m_pendingOptimizationConfig=value;return true;}
    // 바이트와 진입점은 복사한다. 생략한 단계에는 모델 확장의 기본 셰이더를 사용한다.
    bool SetShaders(const ModelShaderDesc&);
    // 마지막 완료된 Compute 스키닝의 GPU 시간이다. 측정값이 없으면 -1이다.
    [[nodiscard]] double GetGpuMilliseconds() const { return m_gpuMilliseconds; }
    // 시점 이동·재시작·비교 복원 직후 이전 변형 위치를 재사용하지 않는다.
    void InvalidateHistory();

private:
    struct MeshBuffer
    {
        std::weak_ptr<const MeshData> source;
        RHI::BufferHandle buffer = nullptr;
    };
    struct GpuSample
    {
        RHI::TimestampQueryHandle query = nullptr;
        RHI::FenceHandle completion;
        Diagnostics::LiveGpuSpan liveSpan;
        std::optional<RHI::TimestampCalibration> calibration;
        std::uint64_t submittedNs=0;
    };
    struct FrameBuffer
    {
        RHI::BufferHandle buffer = nullptr;
        std::vector<uint8_t> bytes;
        RHI::ResourceState state = RHI::ResourceState::Undefined;
    };
    struct FrameSet
    {
        RHI::ResourceSetHandle handle = nullptr;
        RHI::PipelineHandle pipeline = nullptr;
        std::array<RHI::ResourceBinding, 4> bindings = {};
    };
    struct FrameResources
    {
        std::vector<FrameBuffer> buffers;
        std::vector<FrameSet> sets;
        std::vector<uintptr_t> workingSet;
        RHI::FenceHandle completion;
    };
    bool RenderScene(const ModelScene&, const Camera*, const Canvas*, Image*);
    bool ConfigureShaders();
    bool RestoreShaders();
    bool Prepare(const ModelScene&);
    bool PrepareComputePipeline();
    void InitializeLiveProfiling() noexcept;
    void PrepareLiveFrame(Diagnostics::LiveFrameKey) noexcept;
    void PublishLiveAvailability() noexcept;
    RHI::ShaderDesc ShaderDescription(uint32_t slot, const RHI::ShaderDesc& stock) const;
    void ReleaseFrameBuffers();
    void ReleaseFrame(FrameResources&);
    void ReleaseMeshBuffers();
    void CollectGpuSamples();

    Renderer& m_renderer;
    Diagnostics::LiveId m_liveId=0;
    Diagnostics::ControlRegistration m_skinningControl;
    std::array<Diagnostics::ControlRegistration,4> m_optimizationControls;
    std::array<Diagnostics::LiveId,4> m_optimizationRequests{};
    std::array<Diagnostics::ControlRegistration,4> m_liveCatalog;
    SkinningExecutionMode m_mode = SkinningExecutionMode::VertexShader;
    SkinningExecutionMode m_liveAppliedMode = SkinningExecutionMode::VertexShader;
    ModelOptimizationConfig m_optimizationConfig,m_liveAppliedOptimizationConfig;
    std::optional<ModelOptimizationConfig> m_pendingOptimizationConfig;
    std::array<std::vector<uint8_t>, 3> m_shaderBytes;
    std::array<std::string, 3> m_shaderEntries;
    std::array<std::vector<uint8_t>, 2> m_originalVertexBytes;
    std::array<std::string, 2> m_originalVertexEntries;
    std::vector<RHI::ResourceBindingLayout> m_originalVertexBindings;
    uint32_t m_originalConstantBytes = 0;
    bool m_configured = false;
    RHI::ShaderHandle m_computeShader = nullptr;
    RHI::PipelineHandle m_computePipeline = nullptr;
    bool m_computeShaderChanged = true;
    std::vector<MeshBuffer> m_meshBuffers;
    // Lazily grows to maxFramesInFlight; only a completed slot is modified.
    std::vector<FrameResources> m_frames;
    uint32_t m_frameIndex = 0;
    RHI::BufferHandle m_influenceBuffer = nullptr;
    std::vector<SkinInfluence> m_influences;
    std::vector<uint32_t> m_influenceOffsets;
    std::vector<uint32_t> m_influenceCounts;
    std::vector<uint64_t> m_requiredInfluenceJoints;
    std::vector<bool> m_validInfluences;
    std::vector<RendererDrawDesc> m_draws;
    const ModelScene* m_historyScene = nullptr;
    uint32_t m_historyFrame = UINT32_MAX;
    std::vector<RendererDrawDesc> m_historyDraws;
    std::vector<std::weak_ptr<const MeshData>> m_historyMeshes;
    std::vector<size_t> m_historyVertexCounts;
    std::vector<uint64_t> m_geometrySignatures, m_geometryRevisions;
    uint64_t m_nextGeometryRevision = 1;
    std::deque<GpuSample> m_gpuSamples;
    double m_gpuMilliseconds = -1;
};
}
