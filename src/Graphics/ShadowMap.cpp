#include "Graphics/ShadowMap.h"

#include <algorithm>
#include <cmath>
#include <limits>

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

		float Length(const Math::float3& value)
		{
			return std::sqrt(Dot(value, value));
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

		Math::float4x4 Perspective(float fovYRadians, float aspect, float nearPlane, float farPlane)
		{
			const float safeFov = std::max(std::min(fovYRadians, 3.0f), 0.1f);
			const float safeAspect = std::max(aspect, 0.0001f);
			const float f = 1.0f / std::tan(safeFov * 0.5f);

			Math::float4x4 p = {};
			p.m[0] = f / safeAspect;
			p.m[5] = -f;
			p.m[10] = farPlane / (nearPlane - farPlane);
			p.m[11] = -1.0f;
			p.m[14] = (nearPlane * farPlane) / (nearPlane - farPlane);
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

		Math::float3 TransformPoint(const Math::float4x4& matrix, const Math::float3& point)
		{
			return Math::float3(
				matrix.m[0] * point.x + matrix.m[4] * point.y + matrix.m[8] * point.z + matrix.m[12],
				matrix.m[1] * point.x + matrix.m[5] * point.y + matrix.m[9] * point.z + matrix.m[13],
				matrix.m[2] * point.x + matrix.m[6] * point.y + matrix.m[10] * point.z + matrix.m[14]);
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

		// lightDirection is surface-to-light, so place the light camera on that side.
		const Math::float3 lightOrigin(
			desc.sceneCenter.x + lightForward.x * desc.lightDistance,
			desc.sceneCenter.y + lightForward.y * desc.lightDistance,
			desc.sceneCenter.z + lightForward.z * desc.lightDistance);

		const Math::float3 up = SelectUpVector(lightForward);
		const Math::float4x4 view = LookAt(lightOrigin, desc.sceneCenter, up);
		const Math::float4x4 proj = Orthographic(desc.orthoWidth, desc.orthoHeight, desc.nearPlane, desc.farPlane);

		return MultiplyColumnMajor(proj, view);
	}

	Math::float4x4 ComputeSpotLightViewProj(
		const Math::float3& lightPosition,
		const Math::float3& lightDirection,
		const ShadowMapDesc& desc)
	{
		const Math::float3 lightForward = Normalize(lightDirection);
		const Math::float3 target(
			lightPosition.x + lightForward.x,
			lightPosition.y + lightForward.y,
			lightPosition.z + lightForward.z);
		const Math::float4x4 view = LookAt(lightPosition, target, SelectUpVector(lightForward));
		const Math::float4x4 proj = Perspective(desc.spotFovYRadians, 1.0f, desc.nearPlane, desc.farPlane);
		return MultiplyColumnMajor(proj, view);
	}

	ShadowMapDesc FitDirectionalShadowMapToBounds(
		const Math::float3& lightDirection,
		const ShadowMapDesc& baseDesc,
		const Math::float3& boundsMin,
		const Math::float3& boundsMax,
		float padding)
	{
		ShadowMapDesc desc = baseDesc;
		if (boundsMin.x > boundsMax.x || boundsMin.y > boundsMax.y || boundsMin.z > boundsMax.z)
		{
			return desc;
		}

		padding = std::max(padding, 0.0f);
		const Math::float3 center(
			(boundsMin.x + boundsMax.x) * 0.5f,
			(boundsMin.y + boundsMax.y) * 0.5f,
			(boundsMin.z + boundsMax.z) * 0.5f);
		const Math::float3 halfExtent(
			(boundsMax.x - boundsMin.x) * 0.5f,
			(boundsMax.y - boundsMin.y) * 0.5f,
			(boundsMax.z - boundsMin.z) * 0.5f);
		const float radius = std::max(Length(halfExtent), 0.1f);

		const Math::float3 lightForward = Normalize(lightDirection);
		desc.sceneCenter = center;
		desc.lightDistance = std::max(radius + padding + desc.nearPlane, 0.5f);

		const Math::float3 lightOrigin(
			desc.sceneCenter.x + lightForward.x * desc.lightDistance,
			desc.sceneCenter.y + lightForward.y * desc.lightDistance,
			desc.sceneCenter.z + lightForward.z * desc.lightDistance);
		const Math::float4x4 view = LookAt(lightOrigin, desc.sceneCenter, SelectUpVector(lightForward));

		const Math::float3 corners[] = {
			Math::float3(boundsMin.x, boundsMin.y, boundsMin.z),
			Math::float3(boundsMax.x, boundsMin.y, boundsMin.z),
			Math::float3(boundsMin.x, boundsMax.y, boundsMin.z),
			Math::float3(boundsMax.x, boundsMax.y, boundsMin.z),
			Math::float3(boundsMin.x, boundsMin.y, boundsMax.z),
			Math::float3(boundsMax.x, boundsMin.y, boundsMax.z),
			Math::float3(boundsMin.x, boundsMax.y, boundsMax.z),
			Math::float3(boundsMax.x, boundsMax.y, boundsMax.z)
		};

		float minX = std::numeric_limits<float>::max();
		float minY = std::numeric_limits<float>::max();
		float minZ = std::numeric_limits<float>::max();
		float maxX = -std::numeric_limits<float>::max();
		float maxY = -std::numeric_limits<float>::max();
		float maxZ = -std::numeric_limits<float>::max();
		for (const Math::float3& corner : corners)
		{
			const Math::float3 lightSpace = TransformPoint(view, corner);
			minX = std::min(minX, lightSpace.x);
			minY = std::min(minY, lightSpace.y);
			minZ = std::min(minZ, lightSpace.z);
			maxX = std::max(maxX, lightSpace.x);
			maxY = std::max(maxY, lightSpace.y);
			maxZ = std::max(maxZ, lightSpace.z);
		}

		desc.orthoWidth = std::max(maxX - minX + padding * 2.0f, 0.1f);
		desc.orthoHeight = std::max(maxY - minY + padding * 2.0f, 0.1f);
		const float depthRange = std::max(maxZ - minZ + padding * 2.0f, 0.1f);
		desc.farPlane = std::max(desc.nearPlane + depthRange + desc.lightDistance, desc.nearPlane + 0.1f);
		return desc;
	}
}
