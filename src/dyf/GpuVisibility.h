#pragma once

#include "dyf/Camera.h"
#include "dyf/RHI/Buffer.h"
#include "dyf/RHI/RenderGraph.h"
#include <array>
#include <vector>

namespace dyf
{
class GpuVisibility
{
public:
    struct Instance
    {
        Math::float4x4 model={};
        Math::float4x4 previousMVP={};
        Math::float4 centerRadius={};
        // 원본 오차는 0이며 간소화 단계의 오차는 월드 단위다.
        Math::float4 lodErrors={};
        std::array<uint32_t,4> batches={};
        uint32_t lodCount=1,flags=1,padding0=0,padding1=0;
    };
    struct VisibleInstance
    {
        Math::float4x4 model,previousMVP;
    };
    struct Batch
    {
        uint32_t indexCount=0,firstIndex=0;
        int32_t vertexOffset=0;
        // firstInstance는 0이다. 이 시작점에 맞춰 인스턴스 VB를 바인딩한다.
        uint32_t instanceOffset=0,capacity=0;
        std::array<uint32_t,3> padding={};
    };
    struct Settings
    {
        Camera camera;
        uint32_t width=0,height=0;
        float pixelError=1,hysteresis=0.1f;
        bool frustum=true,lod=true,resetHistory=false;
    };

    explicit GpuVisibility(RHI::IDevice& device):m_device(device) {}
    ~GpuVisibility();
    GpuVisibility(const GpuVisibility&)=delete;
    GpuVisibility& operator=(const GpuVisibility&)=delete;
    // 입력 순서가 달라지면 resetHistory를 켜야 이전 객체의 LOD를 재사용하지 않는다.
    // flags의 비트 0이 없으면 경계를 알 수 없는 객체로 취급하여 제거하지 않는다.
    bool Prepare(const std::vector<Instance>&,const std::vector<Batch>&,const Settings&);
    bool RecordUploads(RHI::ICommandList&);
    void CommitUploads() {m_uploadReady=true;m_uploadDirty.fill(false);}
    bool AddPasses(RHI::RenderGraph&);
    bool AddOcclusionPasses(RHI::RenderGraph&,RHI::TextureHandle depth,
        RHI::RGResourceHandle depthHandle,const Camera& jitteredCamera);
    void CommitFrame();

    RHI::BufferHandle GetInstanceBuffer() const {return m_instances;}
    RHI::BufferHandle GetArgumentsBuffer() const {return m_arguments;}
    RHI::RGResourceHandle GetInstancesHandle() const {return m_instancesGraph;}
    RHI::RGResourceHandle GetArgumentsHandle() const {return m_argumentsGraph;}
    uint32_t GetBatchCount() const {return static_cast<uint32_t>(m_batches.size());}
    const Batch& GetBatch(uint32_t index) const {return m_batches[index];}
    // False proves that the unchanged GPU frustum test cannot append an instance.
    // Unknown/invalid bounds stay visible; this does not predict occlusion or LOD.
    bool MayDrawBatch(uint32_t index) const {return index>=m_possibleBatches.size() || m_possibleBatches[index]!=0;}

private:
    struct Frame
    {
        std::array<Math::float4,6> planes={};
        Math::float4 depthRow={};
        Math::float4 projection={};
        std::array<uint32_t,4> counts={};
    };
    struct OcclusionFrame
    {
        Math::float4x4 viewProjection;
        Math::float4 viewport;
        std::array<uint32_t,4> info;
    };
    bool PreparePipeline();
    bool PrepareOcclusion(RHI::TextureHandle);
    void ReleaseOcclusion();
    void ReleaseBuffers();
    bool CreateBuffer(RHI::BufferHandle&,uint64_t bytes,uint32_t stride,RHI::BufferUsage,RHI::ResourceState);
    RHI::IDevice& m_device;
    RHI::ShaderHandle m_shader=nullptr;
    RHI::PipelineHandle m_pipeline=nullptr;
    RHI::ResourceSetHandle m_resources=nullptr;
    RHI::ShaderHandle m_hizShader=nullptr,m_occlusionShader=nullptr;
    RHI::PipelineHandle m_hizPipeline=nullptr,m_occlusionPipeline=nullptr;
    RHI::ResourceSetHandle m_occlusionResources=nullptr;
    std::vector<RHI::ResourceSetHandle> m_hizResources;
    RHI::TextureHandle m_hiz=nullptr,m_hizDepth=nullptr;
    RHI::BufferHandle m_occlusionFrame=nullptr;
    RHI::BufferHandle m_inputs=nullptr,m_batchData=nullptr,m_frame=nullptr;
    RHI::BufferHandle m_instances=nullptr,m_arguments=nullptr,m_lodHistory=nullptr;
    RHI::RGResourceHandle m_instancesGraph,m_argumentsGraph;
    std::vector<Instance> m_inputData;
    std::vector<Batch> m_batches;
    std::vector<uint8_t> m_possibleBatches;
    Frame m_frameData;
    uint32_t m_capacity=0;
    bool m_hizReady=false,m_occlusionReady=false,m_pendingOcclusion=false;
    bool m_uploadReady=false,m_ready=false,m_resetHistory=true,m_pending=false;
    std::array<bool,3> m_uploadDirty={true,true,true};
};
static_assert(sizeof(GpuVisibility::Instance)==192);
static_assert(sizeof(GpuVisibility::VisibleInstance)==128);
static_assert(sizeof(GpuVisibility::Batch)==32);
}
