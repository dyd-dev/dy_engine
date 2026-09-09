#pragma once
#include "Core/Types.h"
#include "Math/Math.h"
#include <array>
#include <vector>

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

	struct alignas(16) SkinInfluence
	{
		std::array<uint32_t, 4> jointIndices = { 0u, 0u, 0u, 0u };
		Math::float4 weights = Math::float4(0.0f, 0.0f, 0.0f, 0.0f);
		float dqBlendWeight = 0.0f;
		std::array<float, 3> _padding = { 0.0f, 0.0f, 0.0f };
	};
	static_assert(sizeof(SkinInfluence) == 48u, "GLSL std430 SkinInfluence must be 48 bytes");

	struct MeshData
	{
		std::vector<Vertex> vertices;
		std::vector<uint32_t> indices;
		std::vector<SkinInfluence> skinInfluences;
	};


	[[nodiscard]] MeshData CreateCubeMesh(float size = 1.0f);
}
