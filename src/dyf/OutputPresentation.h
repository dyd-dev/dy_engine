#pragma once

#include "dyf/RHI/Format.h"
#include "dyf/RHI/RenderGraph.h"

namespace dyf
{
// 완성된 고정 해상도 출력을 창의 중앙에 같은 종횡비로 표시한다.
class OutputPresentation
{
public:
    explicit OutputPresentation(RHI::IDevice& device):m_device(device) {}
    ~OutputPresentation();
    OutputPresentation(const OutputPresentation&)=delete;
    OutputPresentation& operator=(const OutputPresentation&)=delete;

    bool Prepare(uint32_t width,uint32_t height,RHI::Format format);
    RHI::TextureHandle GetTarget() const {return m_target;}
    RHI::ResourceState GetState() const {return m_state;}
    bool AddPass(RHI::RenderGraph&,RHI::RGResourceHandle source,RHI::TextureHandle actualBackBuffer);
    void CommitFrame();

private:
    bool PreparePipeline(RHI::Format);
    RHI::IDevice& m_device;
    RHI::TextureHandle m_target=nullptr;
    RHI::ShaderHandle m_vertex=nullptr,m_fragment=nullptr;
    RHI::PipelineHandle m_pipeline=nullptr;
    RHI::ResourceSetHandle m_resources=nullptr;
    RHI::Format m_format=RHI::Format::Unknown;
    RHI::ResourceState m_state=RHI::ResourceState::Undefined;
    bool m_pending=false;
};
}
