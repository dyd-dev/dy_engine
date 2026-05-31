#include "Graphics/ShadowMath.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace dy::Graphics
{
	namespace
	{
		[[nodiscard]] Math::float3 ProjectPointToPlane(const Math::float3& worldPoint, const ShadowDesc& desc)
		{
			const Math::float3 castDirection(-desc.lightDirection.x, -desc.lightDirection.y, -desc.lightDirection.z);
			const float denom = std::abs(castDirection.z) < 0.001f ? -0.001f : castDirection.z;
			const float t = std::max((desc.receiverPlaneZ - worldPoint.z) / denom, 0.0f);
			return Math::float3(
				worldPoint.x + castDirection.x * t,
				worldPoint.y + castDirection.y * t,
				desc.receiverPlaneZ + desc.bias);
		}

		[[nodiscard]] float Cross2D(const Math::float3& origin, const Math::float3& a, const Math::float3& b)
		{
			return (a.x - origin.x) * (b.y - origin.y) - (a.y - origin.y) * (b.x - origin.x);
		}

		[[nodiscard]] std::vector<Math::float3> BuildConvexHull(std::vector<Math::float3> points)
		{
			std::sort(points.begin(), points.end(), [](const Math::float3& lhs, const Math::float3& rhs) {
				if(lhs.x != rhs.x) return lhs.x < rhs.x;
				return lhs.y < rhs.y;
			});

			points.erase(std::unique(points.begin(), points.end(), [](const Math::float3& lhs, const Math::float3& rhs) {
				return std::abs(lhs.x - rhs.x) < 0.0001f && std::abs(lhs.y - rhs.y) < 0.0001f;
			}), points.end());

			if(points.size() < 3) return {};

			std::vector<Math::float3> hull;
			hull.reserve(points.size() * 2u);
			for(const Math::float3& point : points)
			{
				while(hull.size() >= 2 && Cross2D(hull[hull.size() - 2u], hull.back(), point) <= 0.0f)
				{
					hull.pop_back();
				}
				hull.push_back(point);
			}

			const std::size_t lowerSize = hull.size();
			for(auto it = points.rbegin() + 1; it != points.rend(); ++it)
			{
				while(hull.size() > lowerSize && Cross2D(hull[hull.size() - 2u], hull.back(), *it) <= 0.0f)
				{
					hull.pop_back();
				}
				hull.push_back(*it);
			}

			if(!hull.empty()) hull.pop_back();
			return hull.size() < 3 ? std::vector<Math::float3>{} : hull;
		}

		[[nodiscard]] Math::float3 SelectUpVector(const Math::float3& lightForward)
		{
			// LookAt에서 forward와 up이 평행하면 right 벡터가 0 → 행렬 깨짐.
			// Z-up이 기본, 빛이 거의 수직이면 Y-up으로 폴백.
			if(std::abs(lightForward.z) > 0.95f)
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
		const Math::float4x4 view = Math::LookAtRH(lightOrigin, desc.sceneCenter, up);
		// 그림자 깊이 패스/샘플링은 Y-down 광원 투영 기준으로 튜닝돼 있다(카메라 캐노니컬과 무관).
		Math::float4x4 proj = Math::OrthographicRH_ZO(desc.orthoWidth, desc.orthoHeight, desc.nearPlane, desc.farPlane);
		proj.m[5] = -proj.m[5];

		return proj * view;
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
		const Math::float4x4 view = Math::LookAtRH(lightPosition, target, SelectUpVector(lightForward));
		// 그림자 깊이 패스/샘플링은 Y-down 광원 투영 기준으로 튜닝돼 있다(카메라 캐노니컬과 무관).
		Math::float4x4 proj = Math::PerspectiveRH_ZO(desc.spotFovYRadians, 1.0f, desc.nearPlane, desc.farPlane);
		proj.m[5] = -proj.m[5];
		return proj * view;
	}

	ShadowMapDesc FitDirectionalShadowMapToBounds(
		const Math::float3& lightDirection,
		const ShadowMapDesc& baseDesc,
		const Math::float3& boundsMin,
		const Math::float3& boundsMax,
		float padding)
	{
		ShadowMapDesc desc = baseDesc;
		if(boundsMin.x > boundsMax.x || boundsMin.y > boundsMax.y || boundsMin.z > boundsMax.z)
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
		const Math::float4x4 view = Math::LookAtRH(lightOrigin, desc.sceneCenter, SelectUpVector(lightForward));

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
		for(const Math::float3& corner : corners)
		{
			const Math::float3 lightSpace = Math::TransformPoint(view, corner);
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

	MeshData BuildShadowMesh(const MeshData& sourceMesh, const Math::float4x4& worldMatrix, const ShadowDesc& desc)
	{
		std::vector<Math::float3> projectedPoints;
		projectedPoints.reserve(sourceMesh.vertices.size());
		for(const Vertex& vertex : sourceMesh.vertices)
		{
			projectedPoints.push_back(ProjectPointToPlane(Math::TransformPoint(worldMatrix, vertex.position), desc));
		}

		const std::vector<Math::float3> hull = BuildConvexHull(projectedPoints);
		MeshData shadowMesh = {};
		if(hull.size() < 3) return shadowMesh;

		shadowMesh.vertices.reserve(hull.size());
		for(const Math::float3& point : hull)
		{
			Vertex vertex = {};
			vertex.position = point;
			vertex.normal = Math::float3(0.0f, 0.0f, 1.0f);
			vertex.uv = Math::float2(point.x, point.y);
			shadowMesh.vertices.push_back(vertex);
		}

		shadowMesh.indices.reserve((hull.size() - 2u) * 3u);
		for(uint32_t i = 1; i + 1u < static_cast<uint32_t>(hull.size()); ++i)
		{
			shadowMesh.indices.push_back(0u);
			shadowMesh.indices.push_back(i);
			shadowMesh.indices.push_back(i + 1u);
		}
		return shadowMesh;
	}
}
