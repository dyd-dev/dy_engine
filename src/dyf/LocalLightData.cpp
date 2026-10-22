#include "LocalLightData.h"
#include "SceneAlgorithms.h"
#include "dyf/Scene.h"
#include "dyf/RHI/IDevice.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace dyf
{
namespace
{
template<class Lights>
// 고정 그림자 상수가 사용하는 enabled 광원의 순서를 재현한다. 높은 priority를 먼저 놓고 동점이면 장면 등록 순서를 유지한다.
void SelectShadowSlots(const Lights& lights,std::vector<uint32_t>& selected)
{
    selected.clear();
    selected.reserve(lights.size());
    for(uint32_t index=0;index<lights.size();++index)
        if(lights[index].enabled) selected.push_back(index); // 활성 광원만 고정 슬롯 후보로 넣는다. 실제 그림자 허용 여부는 BuildShadows에서 따로 판단한다.
    std::sort(selected.begin(),selected.end(),[&](uint32_t a,uint32_t b) {
        return lights[a].priority==lights[b].priority ? a<b : lights[a].priority>lights[b].priority;
    });
    if(selected.size()>16) selected.resize(16); // 고정 그림자 배열이 표현하는 최대 16개 슬롯에 맞춘다. 전체 지역 광원을 16개로 제한하는 것은 아니다.
}

// 전체 목록의 원본 인덱스를 고정 그림자 슬롯으로 변환한다. 선택 밖이면 -1을 기록해 대응 그림자가 없음을 표시한다.
float ShadowSlot(const std::vector<uint32_t>& selected,uint32_t index)
{
    const auto found=std::find(selected.begin(),selected.end(),index);
    return found==selected.end() ? -1.f : static_cast<float>(found-selected.begin());
}
}

// 이전 프레임 자료를 비운 뒤 현재 장면의 점광원·스폿 광원을 순서대로 담는다. 벡터 capacity는 재사용할 수 있다.
bool LocalLightData::Prepare(const RHI::IDevice& device,const Scene& scene,bool active)
{
    lights.clear();
    if(!active) return true; // 직접 조명이 비활성이면 빈 광원 목록을 정상 결과로 반환한다.
    const auto& points=scene.PointLights();
    const auto& spots=scene.SpotLights();
    if(points.size()>UINT32_MAX || spots.size()>UINT32_MAX) // 원본 인덱스를 uint32_t로 나타낼 수 없는 크기는 순회 전에 거부한다.
    {
        std::fputs("dyf: Local light indices exceed the renderer's 32-bit range.\n",stderr);
        return false;
    }
    // RHI의 32비트 버퍼 크기와 장치 저장 버퍼 한도 중 작은 값을 실제 최대 크기로 사용한다.
    const uint64_t maximumBytes=std::min<uint64_t>(UINT32_MAX,device.GetLimit(RHI::Limit::StorageBufferBytes));
    const auto maximumLights=maximumBytes/sizeof(RendererClusterLight);
    if(!maximumLights) // 광원 한 항목도 저장할 수 없는 장치는 이 경로를 준비할 수 없다.
    {
        std::fputs("dyf: The device cannot hold a local light storage buffer.\n",stderr);
        return false;
    }
    // 전체 목록을 채우기 전에 종류별 그림자 슬롯을 정한다. 공간 선별 여부와 관계없이 같은 원본 광원이 같은 그림자 인덱스를 갖게 한다.
    SelectShadowSlots(points,m_points);
    SelectShadowSlots(spots,m_spots);
    lights.reserve(static_cast<size_t>(std::min(uint64_t(points.size())+spots.size(),maximumLights)));
    for(uint32_t index=0;index<points.size();++index)
    {
        const auto& light=points[index];
        if(!light.enabled || light.range<=0 || light.intensity<=0) continue; // 꺼져 있거나 양의 범위·강도가 없는 점광원은 전체 평가 목록에서 제외한다.
        if(lights.size()==maximumLights) // 장치 한도에 도달하면 남은 활성 광원을 누락한 채 성공하지 않고 실패를 알린다.
        {
            std::fputs("dyf: Active local lights exceed the device storage-buffer limit.\n",stderr);
            return false;
        }
        // 점광원은 위치/반경·색/강도와 타입 값 2, 대응 그림자 슬롯을 기록한다. 방향·원뿔 필드는 점광원용 기본값이다.
        lights.push_back({{light.position.x,light.position.y,light.position.z,light.range},
            {light.color.x,light.color.y,light.color.z,light.intensity},
            {0,0,-1,-1},{1,2,ShadowSlot(m_points,index),0}});
    }
    for(uint32_t index=0;index<spots.size();++index)
    {
        const auto& light=spots[index];
        if(!light.enabled || light.range<=0 || light.intensity<=0) continue; // 스폿도 같은 활성 조건을 통과해야 목록에 추가한다.
        if(lights.size()==maximumLights) // 점광원과 스폿을 합친 개수가 저장 한도를 넘으면 실패한다.
        {
            std::fputs("dyf: Active local lights exceed the device storage-buffer limit.\n",stderr);
            return false;
        }
        // 스폿 방향은 정규화하고 너무 짧으면 -Z로 대체한다. 원뿔 반각은 비유한 값을 0으로 바꾼 뒤 작은 값/큰 값 순서로 정렬하고 0..89도로 제한해 cos로 저장한다.
        const auto direction=Math::NormalizeOr(light.direction,{0,0,-1});
        const auto cone=EffectiveSpotCone(light.innerConeRadians,light.outerConeRadians);
        // 스폿은 타입 값 3과 바깥/안쪽 원뿔의 cos를 함께 기록한다.
        // 셰이더는 두 벡터의 대응 성분을 곱해 더하는 연산(내적)으로 광원 방향과 픽셀 방향을 비교해 감쇠 구간을 계산한다.
        lights.push_back({{light.position.x,light.position.y,light.position.z,light.range},
            {light.color.x,light.color.y,light.color.z,light.intensity},
            {direction.x,direction.y,direction.z,std::cos(cone.outer)},
            {std::cos(cone.inner),3,ShadowSlot(m_spots,index),0}});
    }
    return true;
}
}
