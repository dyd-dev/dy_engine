#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace dyf
{
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
