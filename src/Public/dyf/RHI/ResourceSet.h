#pragma once

#include <cstdint>

#include "Binding.h"
#include "ResourceHandles.h"

namespace dyf::RHI
{
	// 생성한 세트는 ResourceSetHandle로 바인딩하고 장치의 DestroyResourceSet으로 해제한다.
	struct ResourceSetDesc
	{
		// RHI retains the pipeline and binding resources for this set.
		PipelineHandle pipeline = nullptr;
		const ResourceBinding* bindings = nullptr;
		uint32_t bindingCount = 0;
	};
}
