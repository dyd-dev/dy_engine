#pragma once
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <vector>

namespace dyf
{
    struct SpotConeAngles { float inner,outer; };
    // Both lighting and shadow projection consume the same sorted half-angles.
    // Non-finite inputs have no useful cone; treat each as zero before sorting.
    inline SpotConeAngles EffectiveSpotCone(float inner,float outer)
    {
        if(!std::isfinite(inner)) inner=0;
        if(!std::isfinite(outer)) outer=0;
        constexpr float maximum=1.55334306f; // 89 degrees
        return {std::clamp(std::min(inner,outer),0.f,maximum),
            std::clamp(std::max(inner,outer),0.f,maximum)};
    }
// 내장 셰이더의 배열 크기에 맞춰 enabled/priority 순서로 광원을 선택한다.
// 이 상한을 넘는 광원은 현재 렌더링에서 제외된다.
	template <typename LightType>
	[[nodiscard]] inline std::vector<uint32_t> SelectActiveLightIndices(
		const std::vector<LightType>& lights,
		uint32_t capacity)
	{
		std::vector<uint32_t> indices;
		indices.reserve(lights.size());
		for(uint32_t index = 0u; index < static_cast<uint32_t>(lights.size()); ++index)
		{
			if(lights[index].enabled) indices.push_back(index);
		}
		std::stable_sort(indices.begin(), indices.end(), [&lights](uint32_t left, uint32_t right)
		{
			return lights[left].priority > lights[right].priority;
		});
		if(indices.size() > capacity) indices.resize(capacity);
		return indices;
	}

}
