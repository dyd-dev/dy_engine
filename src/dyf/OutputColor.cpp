#include "OutputColor.h"
#include <cmath>

namespace dyf
{
    // 온도·틴트·선명도 범위와 모든 값의 유한성, gamma의 양수 하한, gain·대비·채도의 비음수를 검사한다. 잘못된 값은 보정하지 않고 false다.
    bool ValidateOutputConfig(const PostProcessConfig& settings)
    {
        const auto finite=[](Math::float3 value) {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        };
        return std::isfinite(settings.temperature) && std::fabs(settings.temperature)<=1 &&
            std::isfinite(settings.tint) && std::fabs(settings.tint)<=1 &&
            std::isfinite(settings.contrast) && settings.contrast>=0 &&
            std::isfinite(settings.saturation) && settings.saturation>=0 &&
            finite(settings.lift) && finite(settings.gamma) && finite(settings.gain) &&
            settings.gamma.x>=.01f && settings.gamma.y>=.01f && settings.gamma.z>=.01f &&
            settings.gain.x>=0 && settings.gain.y>=0 && settings.gain.z>=0 &&
            std::isfinite(settings.sharpness) && settings.sharpness>=0 && settings.sharpness<=1;
    }

    // 5개의 float4 배치로 백색 균형·대비, lift·채도, gamma·선명도, gain·사용 플래그, 역해상도를 반환한다.
    std::array<float,20> OutputConstants(const PostProcessConfig& settings,
        uint32_t width,uint32_t height,bool diagnostic)
    {
        const auto& p=settings;
        // 진단 모드가 아니고 색 보정 값이 기본값과 다를 때만 셰이더의 grading 사용 플래그를 켠다.
        const bool grading=!diagnostic && (p.temperature!=0 || p.tint!=0 || p.contrast!=1 || p.saturation!=1 ||
            p.lift.x!=0 || p.lift.y!=0 || p.lift.z!=0 || p.gamma.x!=1 || p.gamma.y!=1 || p.gamma.z!=1 ||
            p.gain.x!=1 || p.gain.y!=1 || p.gain.z!=1);
        // 온도·틴트를 2의 거듭제곱에 넣어 LMS 세 채널의 양수 배율을 만든다. 둘 다 0이면 배율이 모두 1이므로 백색 균형이 바뀌지 않는다.
        // 진단 모드에서는 색 보정 플래그와 선명도만 끄고 나머지 값은 그대로 전달한다.
        return {std::exp2(p.temperature*.5f+p.tint*.25f),std::exp2(-p.tint*.5f),
            std::exp2(-p.temperature*.5f+p.tint*.25f),p.contrast,
            p.lift.x,p.lift.y,p.lift.z,p.saturation,
            p.gamma.x,p.gamma.y,p.gamma.z,!diagnostic && p.sharpening?p.sharpness:0.f,
            p.gain.x,p.gain.y,p.gain.z,grading?1.f:0.f,
            width?1.f/width:0.f,height?1.f/height:0.f,0,0};
    }
}
