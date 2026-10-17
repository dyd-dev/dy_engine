#pragma once

#include "dyf/RHI/ResourceHandles.h"
#include "dyf/RHI/ResourceState.h"
#include <vector>

namespace dyf::RHI { class IDevice; }
namespace dyf
{
struct Camera;

// 이미 그린 불투명 깊이를 읽으므로 프로브 가림 정보 때문에 장면을 다시 그리지 않는다.
class EnvironmentDepthCapture
{
public:
    explicit EnvironmentDepthCapture(RHI::IDevice& device):m_device(device) {}
    ~EnvironmentDepthCapture();
    bool Read(RHI::TextureHandle depth,const Camera&,float nearPlane,float farPlane,std::vector<float>& distance);
private:
    bool Prepare(RHI::TextureHandle);
    RHI::IDevice& m_device;
    RHI::ShaderHandle m_vertex=nullptr,m_fragment=nullptr;
    RHI::PipelineHandle m_pipeline=nullptr;
    RHI::ResourceSetHandle m_resources=nullptr;
    RHI::TextureHandle m_target=nullptr,m_depth=nullptr;
    RHI::ResourceState m_state=RHI::ResourceState::Undefined;
};
}
