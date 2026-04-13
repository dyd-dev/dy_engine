#pragma once
#include <cstdint>
#include "Math/Math.h"

namespace dy
{
	enum class EntityID   : uint32_t { Invalid = 0xFFFFFFFF };
	enum class MeshID     : uint32_t { Invalid = 0xFFFFFFFF };
	enum class MaterialID : uint32_t { Invalid = 0xFFFFFFFF };
	enum class TextureID  : uint32_t { Invalid = 0xFFFFFFFF };

	// User-Facing Data Components (PODs mapped directly to GPU Buffers)
	
	struct alignas(16) TransformData
	{
		Math::float4x4 worldMatrix;
	};
	struct alignas(16) MaterialData
	{
		Math::float4 albedoColor;
		float roughness;
		float metallic;
		uint32_t albedoTextureIndex; // Bindless texture index
		uint32_t normalTextureIndex; // Bindless texture index
	};
	struct alignas(16) CameraData
	{
		Math::float4x4 viewMatrix;
		Math::float4x4 projectionMatrix;
		Math::float3   worldPosition; // 16-byte
	};
}