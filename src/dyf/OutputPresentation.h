#pragma once

#include "dyf/RHI/Format.h"
#include "dyf/RHI/RenderGraph.h"

namespace dyf
{
// 완성된 고정 해상도 출력을 창의 중앙에 같은 종횡비로 표시한다.
class OutputPresentation
{
public:
    // 장치는 빌려 쓰므로 이 객체보다 오래 살아 있어야 한다. 생성 자체는 GPU 자원을 만들지 않는다.
    explicit OutputPresentation(RHI::IDevice& device):m_device(device) {}
    // 세트·파이프라인·셰이더·중간 타깃을 반납한다. 복사를 금지해 같은 핸들을 중복 소유하지 않게 한다.
    ~OutputPresentation();
    OutputPresentation(const OutputPresentation&)=delete;
    OutputPresentation& operator=(const OutputPresentation&)=delete;

    // 중간 타깃과 복사 파이프라인을 준비한다. 크기 또는 자원 생성이 실패하면 false다.
    bool Prepare(uint32_t width,uint32_t height,RHI::Format format);
    // 렌더러가 완성 영상을 쓸 중간 타깃을 빌려준다. 외부에서 소유권을 가져가는 API가 아니다.
    RHI::TextureHandle GetTarget() const {return m_target;}
    // 다음 그래프 import에 사용할 확정 상태를 반환한다. GPU 완료 상태를 뜻하지 않는다.
    RHI::ResourceState GetState() const {return m_state;}
    // GetTarget을 import한 source를 읽어 실제 백버퍼로 표시하는 패스를 추가한다.
    bool AddPass(RHI::RenderGraph&,RHI::RGResourceHandle source,RHI::TextureHandle actualBackBuffer);
    // 성공한 프레임의 표시 결과 상태를 다음 프레임 시작 상태로 저장한다.
    void CommitFrame();

private:
    // 실제 출력 형식별 파이프라인 준비는 내부에서만 수행한다.
    bool PreparePipeline(RHI::Format);
    RHI::IDevice& m_device;
    RHI::TextureHandle m_target=nullptr;
    RHI::ShaderHandle m_vertex=nullptr,m_fragment=nullptr;
    RHI::PipelineHandle m_pipeline=nullptr;
    RHI::ResourceSetHandle m_resources=nullptr;
    RHI::Format m_format=RHI::Format::Unknown;
    RHI::ResourceState m_state=RHI::ResourceState::Undefined;
    // 이번 그래프에 표시 패스가 추가되었는지 나타낸다. 완료 fence나 제출 성공 자체를 나타내지는 않는다.
    bool m_pending=false;
};
}
