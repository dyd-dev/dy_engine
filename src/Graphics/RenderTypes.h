#pragma once
#include <cstdint>
#include <vector>

#include "Types.h"
#include "Math/Math.h"

namespace dy::Graphics
{
	struct alignas(16) Transform
	{
		Math::float4x4 worldMatrix;
	};
	struct alignas(16) Camera
	{
		Math::float4x4 viewMatrix;
		Math::float4x4 projectionMatrix;
		Math::float3   worldPosition; // 16-byte
	};

	struct Vertex
	{
		Math::float3 position;
		Math::float3 normal;
		Math::float2 uv;
	};

	struct MeshData
	{
		std::vector<Vertex> vertices;
		std::vector<uint32_t> indices;
	};

	struct MaterialData
	{
		Math::float4 baseColor = Math::float4(1.0f, 1.0f, 1.0f, 1.0f);
		TextureID baseColorTex = TextureID::Invalid;
	};

	struct Scene
	{
		std::vector<Transform> m_entityTransforms;
		std::vector<MeshID> m_entityMeshes;
		std::vector<MaterialID> m_entityMaterials;

		std::vector<MeshData> m_meshes;
		std::vector<MaterialData> m_materials;
	};
}