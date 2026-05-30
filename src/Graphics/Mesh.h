#pragma once
#include <cstdint>
#include <vector>

#include "Math/Math.h"
#include "Types.h"

namespace dy::Graphics
{
	struct Vertex
	{
		Math::float3 position;
		Math::float3 normal;
		Math::float2 uv;
	};

	struct MeshSubset
	{
		uint32_t firstVertex = 0;
		uint32_t vertexCount = 0;
		uint32_t firstIndex = 0;
		uint32_t indexCount = 0;
		MaterialID material = MaterialID::Invalid;
	};

	struct MeshData
	{
		std::vector<Vertex> vertices;
		std::vector<uint32_t> indices;
		std::vector<MeshSubset> subsets;
	};
}
