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
		// 선형 RGBA 색상이며 기본색 계수·텍스처에 곱한다. 생략하면 흰색이다.
		dyf::Math::float4 color = dyf::Math::float4(1.0f, 1.0f, 1.0f, 1.0f);
		dyf::Math::float4 tangent = dyf::Math::float4(1.0f, 0.0f, 0.0f, 1.0f);
	};

	// Additional index levels share MeshData::vertices. Error is a cumulative
	// object-space-scaled metric, not a ratio. Generated levels include weighted
	// normal/UV/color error; this is not a strict geometric-distance bound.
	struct MeshLod
	{
		std::vector<uint32_t> indices;
		float error = 0.0f;
	};

	struct MeshData
	{
		std::vector<Vertex> vertices;
		std::vector<uint32_t> indices;
		std::vector<MeshLod> lods;
	};

	// Topology/normal/UV-aware indexed simplification. Leaves the input and
	// vertex ordering unchanged; returns up to three additional levels.
	[[nodiscard]] std::vector<MeshLod> BuildMeshLods(const MeshData&, uint32_t maxLevels = 3);

	[[nodiscard]] MeshData CreateCubeMesh(float size = 1.0f);
}
