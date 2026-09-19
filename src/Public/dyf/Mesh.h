#pragma once
#include "dyf/Types.h"
#include "dyf/Math/Math.h"
#include <vector>

namespace dyf
{
	struct Vertex
	{
		dyf::Math::float3 position;
		dyf::Math::float3 normal;
		dyf::Math::float2 uv;
		// CPU 정점 데이터다. 기본 Renderer의 메시 업로드에는 색상이 포함되지 않는다.
		dyf::Math::float4 color = dyf::Math::float4(1.0f, 1.0f, 1.0f, 1.0f);
		dyf::Math::float4 tangent = dyf::Math::float4(1.0f, 0.0f, 0.0f, 1.0f);
	};

	struct MeshData
	{
		std::vector<Vertex> vertices;
		std::vector<uint32_t> indices;
	};


	[[nodiscard]] MeshData CreateCubeMesh(float size = 1.0f);
}
