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
    // 바이트와 진입점은 복사한다. 생략한 단계에는 모델 확장의 기본 셰이더를 사용한다.
    bool SetShaders(const ModelShaderDesc&);
    // 마지막 완료된 Compute 스키닝의 GPU 시간이다. 측정값이 없으면 -1이다.
    [[nodiscard]] double GetGpuMilliseconds() const { return m_gpuMilliseconds; }

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
    };
    bool RenderScene(const ModelScene&, const Camera*, const Canvas*, Image*);
    bool ConfigureShaders();
    bool RestoreShaders();
    bool Prepare(const ModelScene&);
    bool PrepareComputePipeline();
    RHI::ShaderDesc ShaderDescription(uint32_t slot, const RHI::ShaderDesc& stock) const;
    void ReleaseFrameBuffers();
    void ReleaseMeshBuffers();
    void CollectGpuSamples();

    Renderer& m_renderer;
    SkinningExecutionMode m_mode = SkinningExecutionMode::VertexShader;
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
    std::vector<RHI::BufferHandle> m_frameBuffers;
    std::vector<RendererDrawDesc> m_draws;
    std::deque<GpuSample> m_gpuSamples;
    double m_gpuMilliseconds = -1;
};
}
