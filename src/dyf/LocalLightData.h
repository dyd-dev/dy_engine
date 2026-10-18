#pragma once

#include "ShaderLayout.h"
#include <vector>

namespace dyf::RHI { class IDevice; }
namespace dyf
{
class Scene;

// 공간 분류와 별개인 전체 직접광 목록이다. 고정 그림자 슬롯만 기존 순서를 공유한다.
struct LocalLightData
{
    bool Prepare(const RHI::IDevice&,const Scene&,bool active);
    std::vector<RendererClusterLight> lights;
private:
    std::vector<uint32_t> m_points,m_spots;
};
}
