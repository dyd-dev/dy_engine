#pragma once

#include "dyf/RendererConfig.h"
#include <array>
#include <cstdint>

namespace dyf
{
    bool ValidateOutputConfig(const PostProcessConfig&);
    // 백색 LMS 배율·대비 / lift·채도 / gamma·선명도 / gain·색보정 사용 / 출력 역해상도.
    std::array<float,20> OutputConstants(const PostProcessConfig&,uint32_t width,uint32_t height,
        bool diagnostic=false);
}
