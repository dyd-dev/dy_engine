#include "Graphics/ShadowMap.h"

#include <cmath>

namespace dy::Graphics
{
	namespace
	{
		float Dot(const Math::float3& a, const Math::float3& b)
		{
			return a.x * b.x + a.y * b.y + a.z * b.z;
		}

		Math::float3 Cross(const Math::float3& a, const Math::float3& b)
		{
			return Math::float3(
				a.y * b.z - a.z * b.y,
				a.z * b.x - a.x * b.z,
				a.x * b.y - a.y * b.x);
		}

		Math::float3 Normalize(const Math::float3& v)
		{
			const float len = std::sqrt(Dot(v, v));
			if (len <= 1e-5f) return Math::float3(0.0f, 0.0f, 1.0f);
			return Math::float3(v.x / len, v.y / len, v.z / len);
		}

		Math::float3 Subtract(const Math::float3& a, const Math::float3& b)
		{
			return Math::float3(a.x - b.x, a.y - b.y, a.z - b.z);
		}

		// Right-handed LookAt, column-major. View 변환은 +Z를 카메라 뒤쪽으로 봄.
		Math::float4x4 LookAt(const Math::float3& eye, const Math::float3& target, const Math::float3& up)
		{
			const Math::float3 forward = Normalize(Subtract(target, eye));
			const Math::float3 right   = Normalize(Cross(forward, up));
			const Math::float3 camUp   = Cross(right, forward);

			Math::float4x4 view = Math::float4x4::Identity();
			view.m[0]  = right.x;
			view.m[4]  = right.y;
			view.m[8]  = right.z;
			view.m[12] = -Dot(right, eye);
			view.m[1]  = camUp.x;
			view.m[5]  = camUp.y;
			view.m[9]  = camUp.z;
			view.m[13] = -Dot(camUp, eye);
			view.m[2]  = -forward.x;
			view.m[6]  = -forward.y;
			view.m[10] = -forward.z;
			view.m[14] = Dot(forward, eye);
			return view;
		}

		// Vulkan-friendly Ortho (depth ∈ [0,1]).
		// 본 예제의 main.cpp::CreateOrthographic과 동일한 컨벤션.
		Math::float4x4 Orthographic(float width, float height, float nearPlane, float farPlane)
		{
			Math::float4x4 p = Math::float4x4::Identity();
			p.m[0]  = 2.0f / width;
			p.m[5]  = -2.0f / height;
			p.m[10] = 1.0f / (nearPlane - farPlane);
			p.m[14] = nearPlane / (nearPlane - farPlane);
			return p;
		}

		Math::float4x4 MultiplyColumnMajor(const Math::float4x4& lhs, const Math::float4x4& rhs)
		{
			Math::float4x4 result = {};
			for (int column = 0; column < 4; ++column)
			{
				for (int row = 0; row < 4; ++row)
				{
					float value = 0.0f;
					for (int k = 0; k < 4; ++k)
					{
						value += lhs.m[k * 4 + row] * rhs.m[column * 4 + k];
					}
					result.m[column * 4 + row] = value;
				}
			}
			return result;
		}

		Math::float3 SelectUpVector(const Math::float3& lightForward)
		{
			// LookAt에서 forward와 up이 평행하면 right 벡터가 0 → 행렬 깨짐.
			// Z-up이 기본, 빛이 거의 수직이면 Y-up으로 폴백.
			if (std::abs(lightForward.z) > 0.95f)
			{
				return Math::float3(0.0f, 1.0f, 0.0f);
			}
			return Math::float3(0.0f, 0.0f, 1.0f);
		}
	}

	Math::float4x4 ComputeDirectionalLightViewProj(
		const Math::float3& lightDirection,
		const ShadowMapDesc& desc)
	{
		const Math::float3 lightForward = Normalize(lightDirection);

		// 광원 위치 = sceneCenter에서 lightForward 반대 방향으로 lightDistance만큼 후퇴
		const Math::float3 lightOrigin(
			desc.sceneCenter.x - lightForward.x * desc.lightDistance,
			desc.sceneCenter.y - lightForward.y * desc.lightDistance,
			desc.sceneCenter.z - lightForward.z * desc.lightDistance);

		const Math::float3 up = SelectUpVector(lightForward);
		const Math::float4x4 view = LookAt(lightOrigin, desc.sceneCenter, up);
		const Math::float4x4 proj = Orthographic(desc.orthoWidth, desc.orthoHeight, desc.nearPlane, desc.farPlane);

		return MultiplyColumnMajor(proj, view);
	}
}
