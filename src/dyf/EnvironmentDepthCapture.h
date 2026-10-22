#pragma once

#include "dyf/RHI/Binding.h"
#include <vector>

namespace dyf::RHI { class IDevice; }
namespace dyf
{
struct Camera;

// 이미 그린 불투명 깊이를 읽으므로 프로브 가림 정보 때문에 장면을 다시 그리지 않는다.
class EnvironmentDepthCapture
{
public:
    // 장치를 참조로 보관한다. 이 보조 객체를 소멸할 때까지 장치가 유효해야 한다.
    explicit EnvironmentDepthCapture(RHI::IDevice& device):m_device(device) {}
    ~EnvironmentDepthCapture();
    // 깊이 변환 패스를 제출하고 CPU로 읽어 distance에 행 우선 배열을 쓴다. 실패 시 false이며 출력 일부가 채워졌을 수도 있다.
    bool Read(RHI::TextureHandle depth,const Camera&,float nearPlane,float farPlane,std::vector<float>& distance);
private:
    bool Prepare(RHI::TextureHandle); // 입력 깊이와 출력 크기에 맞게 필요한 자원만 생성하거나 재연결한다.
    RHI::IDevice& m_device;
    RHI::ShaderHandle m_vertex=nullptr,m_fragment=nullptr;
    RHI::PipelineHandle m_pipeline=nullptr;
    RHI::ResourceSetHandle m_resources=nullptr;
    RHI::TextureHandle m_target=nullptr,m_depth=nullptr; // m_target은 소유 출력이고 m_depth는 바인딩 교체 판단에 쓰는 빌린 입력이다.
    RHI::ResourceState m_state=RHI::ResourceState::Undefined; // 다음 그래프에 알려 줄 출력 텍스처의 마지막 제출 상태다.
};
}
