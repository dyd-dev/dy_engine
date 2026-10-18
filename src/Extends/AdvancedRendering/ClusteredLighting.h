#pragma once

#include "LocalLightData.h"
#include "dyf/RHI/Buffer.h"
#include "dyf/RHI/RenderGraph.h"

namespace dyf
{
struct Camera;
class ClusteredLighting
{
public:
    ~ClusteredLighting();
    ClusteredLighting() = default;
    ClusteredLighting(const ClusteredLighting&) = delete;
    ClusteredLighting& operator=(const ClusteredLighting&) = delete;
    // 업로드 목록은 그래프보다 먼저 제출한다. 기록·제출 실패 뒤에는 Reset을 호출한다.
    bool Prepare(RHI::IDevice&, RHI::ICommandList& uploads, const Scene&, const Camera&,
        bool enabled = true, bool active = true);
    // 목록 초과·지원하지 않는 투영·Compute 미지원 시 전체 광원 순회로 누락을 방지한다.
    bool AddPasses(RHI::RenderGraph&);
    void CommitFrame();
    void Reset();
    RHI::BufferHandle GetLights() const { return m_lights; }
    RHI::BufferHandle GetList() const { return m_list; }
    RHI::BufferHandle GetConstants() const { return m_constants; }
    bool IsActive() const { return m_data.counts[1]!=0; }
    RHI::RGResourceHandle GetLightsHandle() const { return m_lightsHandle; }
    RHI::RGResourceHandle GetListHandle() const { return m_listHandle; }
    RHI::RGResourceHandle GetConstantsHandle() const { return m_constantsHandle; }
private:
    bool CreatePipeline();
    bool Upload(RHI::ICommandList&, RHI::BufferHandle&, bool& ready, std::vector<uint8_t>&,
        const void*, uint32_t size, uint32_t stride, RHI::BufferUsage, RHI::ResourceState);
    RHI::IDevice* m_device = nullptr;
    RHI::BufferHandle m_lights = nullptr, m_list = nullptr, m_constants = nullptr;
    RHI::ShaderHandle m_shader = nullptr;
    RHI::PipelineHandle m_pipeline = nullptr;
    RHI::ResourceSetHandle m_set = nullptr;
    std::array<RHI::BufferHandle,3> m_setBuffers = {};
    std::vector<uint8_t> m_lightBytes, m_constantBytes, m_listBytes;
    bool m_lightsReady = false, m_constantsReady = false, m_listReady = false;
    bool m_prepared = false, m_recorded = false;
    RendererClusterConstants m_data = {};
    LocalLightData m_localLights;
    RHI::RGResourceHandle m_lightsHandle, m_listHandle, m_constantsHandle;
};
}
