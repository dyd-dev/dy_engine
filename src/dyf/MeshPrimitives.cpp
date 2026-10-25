#include "dyf/Mesh.h"
#include <algorithm>

namespace dyf
{
	// 장면에 바로 넣을 수 있는 원점 중심 큐브 메시를 만든다.
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

		// 표면에 수직인 방향(법선)을 각 면의 바깥쪽으로 지정한다. 꼭짓점 위치가 같아도 면마다 법선을 따로 써야 조명에서 각진 큐브로 보인다.
		const Math::float3 normals[6] = {
			{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}
		};

		// 표면을 따라가는 방향(접선)과 방향 부호를 넣는다. 법선 텍스처가 표현하는 작은 굴곡을 각 면의 방향에 맞출 때 사용한다.
		const Math::float4 tangents[6] = {
			{1, 0, 0, 1}, {-1, 0, 0, 1}, {0, 0, -1, 1}, {0, 0, 1, 1}, {1, 0, 0, 1}, {1, 0, 0, 1}
		};
		const Math::float2 uvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

		// 각 면 안에서 정점을 가리키는 번호에 그 면의 시작 정점 번호를 더해, 전체 메시에서 삼각형을 연결할 인덱스를 만든다.
		MeshData mesh;
		mesh.vertices.reserve(24);
		mesh.indices.reserve(36);

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

		/* 음수 크기는 0으로 맞추고 큐브 여섯 면마다 정점 네 개를 만든다. 각 정점에는 위치·법선·UV·접선과 방향 부호를 넣는다.
		   면마다 두 삼각형으로 연결해 정점 24개·인덱스 36개의 CPU MeshData를 반환한다.
		   Scene에 등록하거나 GPU로 올리는 작업은 이 결과를 받은 호출부가 맡는다. */
	}
}
