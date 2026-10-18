#pragma once

#include "dyf/RHI/RenderGraph.h"

namespace dyf
{
class AreaLightingLtc
{
public:
    AreaLightingLtc() = default;
    ~AreaLightingLtc();
    AreaLightingLtc(const AreaLightingLtc&) = delete;
    AreaLightingLtc& operator=(const AreaLightingLtc&) = delete;
    // 최초 표 업로드를 그래프보다 먼저 제출한다. 기록·제출 실패 시 Reset으로 폐기한다.
    bool Prepare(RHI::IDevice&,RHI::ICommandList& uploads);
    bool Import(RHI::RenderGraph&);
    void CommitFrame();
    void Reset();
    RHI::TextureHandle GetMatrix() const { return m_matrix; }
    RHI::TextureHandle GetAmplitude() const { return m_amplitude; }
    RHI::RGResourceHandle GetMatrixHandle() const { return m_matrixHandle; }
    RHI::RGResourceHandle GetAmplitudeHandle() const { return m_amplitudeHandle; }
private:
    RHI::IDevice* m_device = nullptr;
    RHI::TextureHandle m_matrix = nullptr,m_amplitude = nullptr;
    RHI::RGResourceHandle m_matrixHandle,m_amplitudeHandle;
    bool m_uploaded = false,m_prepared = false;
};
}
