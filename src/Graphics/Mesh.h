#pragma once
#include "Math/Math.h"
#include <cstdint>
#include <vector>
#include <string>

namespace dy::Graphics
{
	struct Vertex
	{
		dy::Math::float3 position;
		dy::Math::float3 normal;
		dy::Math::float2 uv;
		dy::Math::float4 tangent = dy::Math::float4(1.0f, 0.0f, 0.0f, 1.0f);
	};

	struct MeshData
	{
		std::vector<Vertex> vertices;
		std::vector<uint32_t> indices;
	};

	class Mesh
	{
	public:
		static bool LoadFromOBJ(const std::string& path, MeshData& outData);
	};
}
