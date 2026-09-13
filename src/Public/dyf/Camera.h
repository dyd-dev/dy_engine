#pragma once

#include "dyf/Math/Math.h"
#include <cstdio>

namespace dyf
{
	struct Camera
	{
		Math::float4x4 view = Math::LookAtRH({0, -3, 2}, {0, 0, 0}, {0, 0, 1});
		Math::float4x4 projection = Math::PerspectiveRH_ZO(1.04719755f, 1.0f, .1f, 100.0f);
		Math::float3 position = {0, -3, 2};

		bool LookAt(Math::float3 eye, Math::float3 target = {0, 0, 0}, Math::float3 up = {0, 0, 1})
		{
			const auto forward = target - eye;
			if(!std::isfinite(eye.x + eye.y + eye.z + target.x + target.y + target.z + up.x + up.y + up.z)
				|| Math::LengthSquared(forward) <= 1.0e-8f
				|| Math::LengthSquared(Math::Cross(forward, up)) <= 1.0e-8f)
				{ std::fprintf(stderr, "dyf: Invalid camera pose.\n"); return false; }
			view = Math::LookAtRH(eye, target, up);
			position = eye;
			return true;
		}

		bool SetPerspective(float aspect = 1.0f, float fovYRadians = 1.04719755f,
			float nearPlane = .1f, float farPlane = 100.0f)
		{
			if(!std::isfinite(aspect + fovYRadians + nearPlane + farPlane)
				|| aspect <= 0 || fovYRadians <= 0 || fovYRadians >= 3.14159265f
				|| nearPlane <= 0 || farPlane <= nearPlane)
				{ std::fprintf(stderr, "dyf: Invalid perspective camera.\n"); return false; }
			const float f = 1.0f / std::tan(fovYRadians * .5f);
			projection = {};
			projection.m[0] = f / aspect;
			projection.m[5] = f;
			projection.m[10] = farPlane / (nearPlane - farPlane);
			projection.m[11] = -1.0f;
			projection.m[14] = nearPlane * farPlane / (nearPlane - farPlane);
			return true;
		}

		bool SetOrthographic(float width, float height, float nearPlane = .1f, float farPlane = 100.0f)
		{
			if(!std::isfinite(width + height + nearPlane + farPlane)
				|| width <= 0 || height <= 0 || farPlane <= nearPlane)
				{ std::fprintf(stderr, "dyf: Invalid orthographic camera.\n"); return false; }
			projection = Math::OrthographicRH_ZO(width, height, nearPlane, farPlane);
			return true;
		}
	};
}
