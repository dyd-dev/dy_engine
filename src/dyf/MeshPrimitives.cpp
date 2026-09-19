#include "dyf/Mesh.h"
#include <algorithm>

namespace dyf
{
	MeshData CreateCubeMesh(float size)
	{
		const float h = std::max(size, 0.0f) * 0.5f;
		const Math::float3 positions[6][4] = {
			{{-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}},
			{{h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h}},
			{{h, -h, h}, {h, -h, -h}, {h, h, -h}, {h, h, h}},
			{{-h, -h, -h}, {-h, -h, h}, {-h, h, h}, {-h, h, -h}},
			{{-h, h, h}, {h, h, h}, {h, h, -h}, {-h, h, -h}},
			{{-h, -h, -h}, {h, -h, -h}, {h, -h, h}, {-h, -h, h}}
		};
		const Math::float3 normals[6] = {
			{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}
		};
		const Math::float4 tangents[6] = {
			{1, 0, 0, 1}, {-1, 0, 0, 1}, {0, 0, -1, 1}, {0, 0, 1, 1}, {1, 0, 0, 1}, {1, 0, 0, 1}
		};
		const Math::float2 uvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

		MeshData mesh;
		mesh.vertices.reserve(24);
		mesh.indices.reserve(36);
		// 각 면의 정점은 법선과 UV가 달라 따로 저장한다.
		for(uint32_t face = 0; face < 6; ++face)
		{
			const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
			for(uint32_t corner = 0; corner < 4; ++corner)
			{
				Vertex vertex;
				vertex.position = positions[face][corner];
				vertex.normal = normals[face];
				vertex.uv = uvs[corner];
				vertex.tangent = tangents[face];
				mesh.vertices.push_back(vertex);
			}
			for(uint32_t index : {0u, 1u, 2u, 0u, 2u, 3u})
				mesh.indices.push_back(base + index);
		}
		return mesh;
	}
}
