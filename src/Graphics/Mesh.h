#pragma once
#include "Math/Math.h"
#include <vector>
#include <string>

namespace dy::Graphics
{
	struct Vertex
	{
		dy::Math::Vector3 position;
		dy::Math::Vector3 normal;
		dy::Math::Vector2 uv;
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
