#pragma once
#include "GpuVisibility.h"

namespace dyf {
class GpuShadowVisibility {
public:
    using Batch=GpuVisibility::Batch;
    using Instance=GpuVisibility::VisibleInstance;
    struct Input {
        Math::float4 minimum={},maximum={};
        std::array<uint32_t,4> ids={}; // entity, batch; minimum.w enables the local bound.
    };
    struct View {
        Math::float4x4 projection={};
        std::array<Math::float4,18> receiverPlanes={};
        std::array<uint32_t,4> info={}; // receiver-plane count, dirty.
    };
    explicit GpuShadowVisibility(RHI::IDevice& device):m_device(device) {}
    ~GpuShadowVisibility();
    GpuShadowVisibility(const GpuShadowVisibility&)=delete;
    GpuShadowVisibility& operator=(const GpuShadowVisibility&)=delete;
    static bool Supported(const RHI::IDevice&);
    bool Prepare(const std::vector<Input>&,const std::vector<Batch>&,const std::vector<View>&,RHI::BufferHandle transforms);
    bool RecordUploads(RHI::ICommandList&);
    bool AddPasses(RHI::RenderGraph&);
    void CommitFrame();
    RHI::BufferHandle Instances() const {return m_instances;}
    RHI::BufferHandle Arguments() const {return m_arguments;}
    RHI::RGResourceHandle InstancesResource() const {return m_instancesGraph;}
    RHI::RGResourceHandle ArgumentsResource() const {return m_argumentsGraph;}
private:
    bool PreparePipeline();
    void ReleaseBuffers();
    RHI::IDevice& m_device;
    RHI::ShaderHandle m_shader=nullptr;
    RHI::PipelineHandle m_pipeline=nullptr;
    RHI::ResourceSetHandle m_resources=nullptr;
    RHI::BufferHandle m_inputs=nullptr,m_batches=nullptr,m_views=nullptr,m_models=nullptr;
    RHI::BufferHandle m_instances=nullptr,m_arguments=nullptr,m_source=nullptr;
    RHI::RGResourceHandle m_instancesGraph,m_argumentsGraph;
    std::vector<Input> m_inputData;
    std::vector<Batch> m_batchData;
    std::vector<View> m_viewData;
    uint32_t m_capacity=0;
    bool m_uploadReady=false,m_outputReady=false,m_pending=false;
    std::array<bool,3> m_dirty={true,true,true};
};
static_assert(sizeof(GpuShadowVisibility::Input)==48);
static_assert(sizeof(GpuShadowVisibility::View)==368);
}
