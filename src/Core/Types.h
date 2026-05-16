#pragma once
#include <cstdint>
#include <vector>

#include "Math/Math.h"

namespace dy
{
	enum class EntityID   : uint32_t { Invalid = 0xFFFFFFFF };
	enum class MeshID     : uint32_t { Invalid = 0xFFFFFFFF };
	enum class MaterialID : uint32_t { Invalid = 0xFFFFFFFF };
	enum class TextureID  : uint32_t { Invalid = 0xFFFFFFFF };

	template <typename T>
	[[nodiscard]] constexpr uint32_t ToIndex(T id) { return static_cast<uint32_t>(id); }
	template <typename T>
	[[nodiscard]] constexpr bool IsValid(T id) { return ToIndex(id) != 0xFFFFFFFFu; }

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
		float px = 0.0f;
		float py = 0.0f;
		float pz = 0.0f;
		float u = 0.0f;
		float v = 0.0f;
		float nx = 0.0f;
		float ny = 0.0f;
		float nz = 1.0f;
		float tx = 1.0f;
		float ty = 0.0f;
		float tz = 0.0f;
		float tw = 1.0f;
	};
	struct Mesh
	{
		std::vector<Vertex> vertices;
		std::vector<uint32_t> indices;
	};
	struct RenderFlags
	{
		bool castShadow = true;
		bool receiveShadow = true;
	};
	struct Material
	{
		Math::float4 baseColor = Math::float4(1.0f, 1.0f, 1.0f, 1.0f);
		TextureID baseColorTexture = TextureID::Invalid;
		Math::float3 emissiveColor = Math::float3(0.0f, 0.0f, 0.0f);
		float metallicFactor = 0.0f;
		float roughnessFactor = 0.5f;
		float normalScale = 1.0f;
		float occlusionStrength = 1.0f;
		TextureID metallicRoughnessTexture = TextureID::Invalid;
		TextureID normalTexture = TextureID::Invalid;
		TextureID occlusionTexture = TextureID::Invalid;
		TextureID emissiveTexture = TextureID::Invalid;
	};
}
