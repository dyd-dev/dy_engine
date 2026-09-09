#pragma once

#include "Math/Math.h"

namespace dy::Graphics
{
	struct Camera
	{
		Math::float4x4 view = Math::float4x4::Identity();
		Math::float4x4 projection = Math::float4x4::Identity();
		Math::float3 position = Math::float3(0.0f, 0.0f, 0.0f);
	};
}
