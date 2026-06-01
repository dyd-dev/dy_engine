#pragma once
#include "Core/Types.h"
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
		dy::Math::float4 color = dy::Math::float4(1.0f, 1.0f, 1.0f, 1.0f);
		dy::Math::float4 tangent = dy::Math::float4(1.0f, 0.0f, 0.0f, 1.0f);
	};

	struct MeshData
	{
		std::vector<Vertex> vertices;
		std::vector<uint32_t> indices;
	};

	struct ObjMaterialInfo
	{
		MaterialDesc material;
		std::string baseColorTexturePath;
		bool hasBaseColor = false;
		bool hasBaseColorTexture = false;
	};

	struct ObjLoadOptions
	{
		bool flipV = false;
	};

	class Mesh
	{
	public:
		static bool LoadFromOBJ(const std::string& path, MeshData& outData);
		static bool LoadFromOBJ(
			const std::string& path,
			MeshData& outData,
			ObjMaterialInfo* outMaterial,
			const ObjLoadOptions& options = {});
	};

	[[nodiscard]] dy::Mesh ToRenderMesh(const MeshData& meshData);
}
