#pragma once
#include "Graphics/Mesh.h"
#include "Math/Math.h"

namespace dy::Graphics
{
	struct ShadowDesc
	{
		Math::float3 lightDirection = Math::float3(0.0f, 0.0f, 1.0f);
		float receiverPlaneZ = 0.0f;
		float bias = 0.001f;
	};

	[[nodiscard]] MeshData BuildShadowMesh(
		const MeshData& sourceMesh,
		const Math::float4x4& worldMatrix,
		const ShadowDesc& desc);
}
