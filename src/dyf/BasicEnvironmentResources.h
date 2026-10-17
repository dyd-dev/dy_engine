#pragma once

#include "dyf/RenderResources.h"

namespace dyf
{
// 환경 모듈을 장착하지 않은 기본 PBR 경로의 중립 입력만 소유한다.
class BasicEnvironmentResources
{
public:
    ~BasicEnvironmentResources();
    bool Prepare(RHI::IDevice&,RHI::ICommandList&);
    void CommitUploads() { m_ready=true; }
    RenderEnvironmentResources GetResources(RHI::TextureHandle black) const;
private:
    RHI::IDevice* m_device=nullptr;
    std::array<RHI::BufferHandle,3> m_buffers={};
    bool m_ready=false;
};
}
