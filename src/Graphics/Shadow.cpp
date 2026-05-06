#include "Graphics/Shadow.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace dy::Graphics
{
	namespace
	{
		[[nodiscard]] Math::float3 TransformPoint(const Math::float4x4& matrix, const Math::float3& point)
		{
			return Math::float3(
				matrix.m[0] * point.x + matrix.m[4] * point.y + matrix.m[8] * point.z + matrix.m[12],
				matrix.m[1] * point.x + matrix.m[5] * point.y + matrix.m[9] * point.z + matrix.m[13],
				matrix.m[2] * point.x + matrix.m[6] * point.y + matrix.m[10] * point.z + matrix.m[14]
			);
		}

		[[nodiscard]] Math::float3 ProjectPointToPlane(const Math::float3& worldPoint, const ShadowDesc& desc)
		{
			const Math::float3 castDirection(-desc.lightDirection.x, -desc.lightDirection.y, -desc.lightDirection.z);
			const float denom = std::abs(castDirection.z) < 0.001f ? -0.001f : castDirection.z;
			const float t = std::max((desc.receiverPlaneZ - worldPoint.z) / denom, 0.0f);
			return Math::float3(
				worldPoint.x + castDirection.x * t,
				worldPoint.y + castDirection.y * t,
				desc.receiverPlaneZ + desc.bias
			);
		}

		[[nodiscard]] float Cross2D(const Math::float3& origin, const Math::float3& a, const Math::float3& b)
		{
			return (a.x - origin.x) * (b.y - origin.y) - (a.y - origin.y) * (b.x - origin.x);
		}

		[[nodiscard]] std::vector<Math::float3> BuildConvexHull(std::vector<Math::float3> points)
		{
			std::sort(points.begin(), points.end(), [](const Math::float3& lhs, const Math::float3& rhs) {
				if (lhs.x != rhs.x) return lhs.x < rhs.x;
				return lhs.y < rhs.y;
			});

			points.erase(std::unique(points.begin(), points.end(), [](const Math::float3& lhs, const Math::float3& rhs) {
				return std::abs(lhs.x - rhs.x) < 0.0001f && std::abs(lhs.y - rhs.y) < 0.0001f;
			}), points.end());

			if (points.size() < 3) return {};

			std::vector<Math::float3> hull;
			hull.reserve(points.size() * 2);
			for (const Math::float3& point : points)
			{
				while (hull.size() >= 2 && Cross2D(hull[hull.size() - 2], hull.back(), point) <= 0.0f)
				{
					hull.pop_back();
				}
				hull.push_back(point);
			}

			const std::size_t lowerSize = hull.size();
			for (auto it = points.rbegin() + 1; it != points.rend(); ++it)
			{
				while (hull.size() > lowerSize && Cross2D(hull[hull.size() - 2], hull.back(), *it) <= 0.0f)
				{
					hull.pop_back();
				}
				hull.push_back(*it);
			}

			if (!hull.empty()) hull.pop_back();
			return hull.size() < 3 ? std::vector<Math::float3>{} : hull;
		}
	}

	MeshData BuildShadowMesh(const MeshData& sourceMesh, const Math::float4x4& worldMatrix, const ShadowDesc& desc)
	{
		std::vector<Math::float3> projectedPoints;
		projectedPoints.reserve(sourceMesh.vertices.size());
		for (const Vertex& vertex : sourceMesh.vertices)
		{
			const Math::float3 worldPoint = TransformPoint(worldMatrix, vertex.position);
			projectedPoints.push_back(ProjectPointToPlane(worldPoint, desc));
		}

		const std::vector<Math::float3> hull = BuildConvexHull(projectedPoints);
		MeshData shadowMesh;
		if (hull.size() < 3) return shadowMesh;

		shadowMesh.vertices.reserve(hull.size());
		for (const Math::float3& point : hull)
		{
			Vertex vertex = {};
			vertex.position = point;
			vertex.normal = Math::float3(0.0f, 0.0f, 1.0f);
			vertex.uv = Math::float2(point.x, point.y);
			shadowMesh.vertices.push_back(vertex);
		}

		shadowMesh.indices.reserve((hull.size() - 2) * 3);
		for (uint32_t i = 1; i + 1 < static_cast<uint32_t>(hull.size()); ++i)
		{
			shadowMesh.indices.push_back(0);
			shadowMesh.indices.push_back(i);
			shadowMesh.indices.push_back(i + 1);
		}

		return shadowMesh;
	}
}
