#include "OutputColor.h"
#include <cmath>

namespace dyf
{
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

    std::array<float,20> OutputConstants(const PostProcessConfig& settings,
        uint32_t width,uint32_t height,bool diagnostic)
    {
        const auto& p=settings;
        const bool grading=!diagnostic && (p.temperature!=0 || p.tint!=0 || p.contrast!=1 || p.saturation!=1 ||
            p.lift.x!=0 || p.lift.y!=0 || p.lift.z!=0 || p.gamma.x!=1 || p.gamma.y!=1 || p.gamma.z!=1 ||
            p.gain.x!=1 || p.gain.y!=1 || p.gain.z!=1);
        return {std::exp2(p.temperature*.5f+p.tint*.25f),std::exp2(-p.tint*.5f),
            std::exp2(-p.temperature*.5f+p.tint*.25f),p.contrast,
            p.lift.x,p.lift.y,p.lift.z,p.saturation,
            p.gamma.x,p.gamma.y,p.gamma.z,!diagnostic && p.sharpening?p.sharpness:0.f,
            p.gain.x,p.gain.y,p.gain.z,grading?1.f:0.f,
            width?1.f/width:0.f,height?1.f/height:0.f,0,0};
    }
}
