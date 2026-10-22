#pragma once

#include "ShaderLayout.h"
#include <vector>

namespace dyf::RHI { class IDevice; }
namespace dyf
{
class Scene;

// 공간 분류와 별개로 점광원·스폿 광원 목록을 만든다. 목록 자체는 장면 순서로 채우고, 각 광원의 그림자 슬롯 번호만 종류별 우선순위 선택에 맞춘다.
struct LocalLightData
{
    // 활성 여부·장치 저장 버퍼 한도를 확인하고 매번 목록을 다시 채운다. false를 받으면 부분 목록을 완성된 입력으로 사용하면 안 된다.
    bool Prepare(const RHI::IDevice&,const Scene&,bool active);
    std::vector<RendererClusterLight> lights; // 점광원 다음 스폿 광원 순서의 셰이더 자료다. 기본 조명 또는 조명 확장이 업로드한다.
private:
    std::vector<uint32_t> m_points,m_spots; // 종류별 고정 그림자 배열의 우선순위 슬롯을 재현하기 위한 원본 인덱스 임시 저장소다.
};
}
