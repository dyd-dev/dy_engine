#include "Graphics/ModelLoaderInternal.h"

#include <cmath>
#include <vector>

namespace dy::Graphics::ModelLoaderInternal
{
	Math::float3 BuildFallbackTangent(const Math::float3& normal)
	{
		const Math::float3 up = std::fabs(normal.z) < 0.999f
			? Math::float3(0.0f, 0.0f, 1.0f)
			: Math::float3(0.0f, 1.0f, 0.0f);
		return Math::NormalizeOr(
			Math::Cross(up, normal),
			Math::float3(1.0f, 0.0f, 0.0f));
	}

	void CalculateTangents(MeshData& data, bool generateMissingNormals)
	{
		std::vector<Math::float3> tangents(data.vertices.size(), Math::float3(0.0f, 0.0f, 0.0f));
		std::vector<Math::float3> bitangents(data.vertices.size(), Math::float3(0.0f, 0.0f, 0.0f));
		std::vector<Math::float3> normals;
		if(generateMissingNormals)
			normals.assign(data.vertices.size(), Math::float3(0.0f, 0.0f, 0.0f));

		for(size_t i = 0; i + 2 < data.indices.size(); i += 3)
		{
			const uint32_t i0 = data.indices[i + 0];
			const uint32_t i1 = data.indices[i + 1];
			const uint32_t i2 = data.indices[i + 2];
			if(i0 >= data.vertices.size() || i1 >= data.vertices.size() || i2 >= data.vertices.size()) continue;

			const Vertex& v0 = data.vertices[i0];
			const Vertex& v1 = data.vertices[i1];
			const Vertex& v2 = data.vertices[i2];
			const Math::float3 edge1 = v1.position - v0.position;
			const Math::float3 edge2 = v2.position - v0.position;
			if(generateMissingNormals)
			{
				const Math::float3 faceNormal = Math::Cross(edge1, edge2);
				normals[i0] = normals[i0] + faceNormal;
				normals[i1] = normals[i1] + faceNormal;
				normals[i2] = normals[i2] + faceNormal;
			}

			const float du1 = v1.uv.x - v0.uv.x;
			const float dv1 = v1.uv.y - v0.uv.y;
			const float du2 = v2.uv.x - v0.uv.x;
			const float dv2 = v2.uv.y - v0.uv.y;
			const float determinant = du1 * dv2 - du2 * dv1;
			if(std::fabs(determinant) <= 1.0e-8f) continue;

			const float inverseDeterminant = 1.0f / determinant;
			const Math::float3 tangent = ((edge1 * dv2) - (edge2 * dv1)) * inverseDeterminant;
			const Math::float3 bitangent = ((edge2 * du1) - (edge1 * du2)) * inverseDeterminant;
			tangents[i0] = tangents[i0] + tangent;
			tangents[i1] = tangents[i1] + tangent;
			tangents[i2] = tangents[i2] + tangent;
			bitangents[i0] = bitangents[i0] + bitangent;
			bitangents[i1] = bitangents[i1] + bitangent;
			bitangents[i2] = bitangents[i2] + bitangent;
		}

		for(size_t i = 0; i < data.vertices.size(); ++i)
		{
			Vertex& vertex = data.vertices[i];
			const Math::float3 sourceNormal = generateMissingNormals ? normals[i] : vertex.normal;
			const Math::float3 normal = Math::NormalizeOr(
				sourceNormal,
				Math::float3(0.0f, 0.0f, 1.0f));
			const Math::float3 rawTangent = tangents[i];
			const Math::float3 orthogonalTangent = rawTangent
				- normal * Math::Dot(normal, rawTangent);
			const Math::float3 tangent = Math::NormalizeOr(
				orthogonalTangent,
				BuildFallbackTangent(normal));
			const float handedness = Math::Dot(
				Math::Cross(normal, tangent),
				bitangents[i]) < 0.0f ? -1.0f : 1.0f;
			vertex.normal = normal;
			vertex.tangent = Math::float4(tangent.x, tangent.y, tangent.z, handedness);
		}
	}
}
