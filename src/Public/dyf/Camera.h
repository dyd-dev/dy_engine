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
			const auto finite=[](Math::float3 v) {return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);};
			if(!finite(eye) || !finite(target) || !finite(up) || !finite(forward)
				|| Math::LengthSquared(Math::Cross(Math::Normalize(forward), Math::Normalize(up))) <= 1.0e-8f)
				{ std::fprintf(stderr, "dyf: Invalid camera pose.\n"); return false; }
			const auto candidate = Math::LookAtRH(eye, target, up);
			for(float value:candidate.m) if(!std::isfinite(value))
				{ std::fprintf(stderr, "dyf: Invalid camera pose.\n"); return false; }
			view = candidate;
			position = eye;
			return true;
		}

		bool SetPerspective(float aspect = 1.0f, float fovYRadians = 1.04719755f,
			float nearPlane = .1f, float farPlane = 100.0f)
		{
			if(!std::isfinite(aspect) || !std::isfinite(fovYRadians) || !std::isfinite(nearPlane) || !std::isfinite(farPlane)
				|| aspect <= 0 || fovYRadians <= 0 || fovYRadians >= 3.14159265f
				|| nearPlane <= 0 || farPlane <= nearPlane)
				{ std::fprintf(stderr, "dyf: Invalid perspective camera.\n"); return false; }
			const double f = 1.0 / std::tan(double(fovYRadians) * .5);
			const double depth = double(nearPlane) - farPlane;
			Math::float4x4 candidate = {};
			candidate.m[0] = float(f / aspect);
			candidate.m[5] = float(f);
			candidate.m[10] = float(farPlane / depth);
			candidate.m[11] = -1.0f;
			candidate.m[14] = float(double(nearPlane) * farPlane / depth);
			for(float value:candidate.m) if(!std::isfinite(value))
				{ std::fprintf(stderr, "dyf: Invalid perspective camera.\n"); return false; }
			projection = candidate;
			return true;
		}

		bool SetOrthographic(float width, float height, float nearPlane = .1f, float farPlane = 100.0f)
		{
			if(!std::isfinite(width) || !std::isfinite(height) || !std::isfinite(nearPlane) || !std::isfinite(farPlane)
				|| width <= 0 || height <= 0 || farPlane <= nearPlane)
				{ std::fprintf(stderr, "dyf: Invalid orthographic camera.\n"); return false; }
			const double depth = double(nearPlane) - farPlane;
			Math::float4x4 candidate = {};
			candidate.m[0] = float(2.0 / width);
			candidate.m[5] = float(2.0 / height);
			candidate.m[10] = float(1.0 / depth);
			candidate.m[14] = float(nearPlane / depth);
			candidate.m[15] = 1;
			for(float value:candidate.m) if(!std::isfinite(value))
				{ std::fprintf(stderr, "dyf: Invalid orthographic camera.\n"); return false; }
			projection = candidate;
			return true;
		}
	};
}
