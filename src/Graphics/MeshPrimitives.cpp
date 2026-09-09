#include "Graphics/Mesh.h"
#include <algorithm>

namespace dy::Graphics
{
	MeshData CreateCubeMesh(float size)
	{
		const float h = std::max(size, 0.0f) * 0.5f;
		MeshData mesh = {};
		mesh.vertices.reserve(24u);
		mesh.indices.reserve(36u);

		auto addFace = [&](const Math::float3& a, const Math::float3& b, const Math::float3& c, const Math::float3& d, const Math::float3& normal, const Math::float4& tangent)
		{
			const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
			Vertex v0 = {};
			Vertex v1 = {};
			Vertex v2 = {};
			Vertex v3 = {};
			v0.position = a;
			v1.position = b;
			v2.position = c;
			v3.position = d;
			v0.normal = normal;
			v1.normal = normal;
			v2.normal = normal;
			v3.normal = normal;
			v0.tangent = tangent;
			v1.tangent = tangent;
			v2.tangent = tangent;
			v3.tangent = tangent;
			v0.uv = Math::float2(0.0f, 0.0f);
			v1.uv = Math::float2(1.0f, 0.0f);
			v2.uv = Math::float2(1.0f, 1.0f);
			v3.uv = Math::float2(0.0f, 1.0f);
			mesh.vertices.push_back(v0);
			mesh.vertices.push_back(v1);
			mesh.vertices.push_back(v2);
			mesh.vertices.push_back(v3);
			mesh.indices.push_back(base + 0u);
			mesh.indices.push_back(base + 1u);
			mesh.indices.push_back(base + 2u);
			mesh.indices.push_back(base + 0u);
			mesh.indices.push_back(base + 2u);
			mesh.indices.push_back(base + 3u);
		};

		addFace(Math::float3(-h, -h, h), Math::float3(h, -h, h), Math::float3(h, h, h), Math::float3(-h, h, h), Math::float3(0.0f, 0.0f, 1.0f), Math::float4(1.0f, 0.0f, 0.0f, 1.0f));
		addFace(Math::float3(h, -h, -h), Math::float3(-h, -h, -h), Math::float3(-h, h, -h), Math::float3(h, h, -h), Math::float3(0.0f, 0.0f, -1.0f), Math::float4(-1.0f, 0.0f, 0.0f, 1.0f));
		addFace(Math::float3(h, -h, h), Math::float3(h, -h, -h), Math::float3(h, h, -h), Math::float3(h, h, h), Math::float3(1.0f, 0.0f, 0.0f), Math::float4(0.0f, 0.0f, -1.0f, 1.0f));
		addFace(Math::float3(-h, -h, -h), Math::float3(-h, -h, h), Math::float3(-h, h, h), Math::float3(-h, h, -h), Math::float3(-1.0f, 0.0f, 0.0f), Math::float4(0.0f, 0.0f, 1.0f, 1.0f));
		addFace(Math::float3(-h, h, h), Math::float3(h, h, h), Math::float3(h, h, -h), Math::float3(-h, h, -h), Math::float3(0.0f, 1.0f, 0.0f), Math::float4(1.0f, 0.0f, 0.0f, 1.0f));
		addFace(Math::float3(-h, -h, -h), Math::float3(h, -h, -h), Math::float3(h, -h, h), Math::float3(-h, -h, h), Math::float3(0.0f, -1.0f, 0.0f), Math::float4(1.0f, 0.0f, 0.0f, 1.0f));
		return mesh;
	}
}
