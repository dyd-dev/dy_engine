#pragma once

#include "dyf/Renderer.h"
#include <array>
#include <cstdint>

namespace dyf
{
    // 유한성·허용 범위를 검사해 설정 적용 경계에서 잘못된 입력을 거부할 때 사용한다.
    bool ValidateOutputConfig(const PostProcessConfig&);
    // 백색 LMS 배율·대비 / lift·채도 / gamma·선명도 / gain·색보정 사용 / 출력 역해상도.
    // 반환값의 순서는 셰이더 상수 배치와 계약이다. diagnostic이면 색 보정/선명도 적용 플래그를 끈다.
    std::array<float,20> OutputConstants(const PostProcessConfig&,uint32_t width,uint32_t height,
        bool diagnostic=false);
}
