#pragma once
#include <cstdint>

#include "Math/Math.h"

namespace dy
{
	enum class EntityID            : uint32_t { Invalid = 0xFFFFFFFF };
	enum class MeshID              : uint32_t { Invalid = 0xFFFFFFFF };
	enum class MaterialID          : uint32_t { Invalid = 0xFFFFFFFF };
	enum class TextureID           : uint32_t { Invalid = 0xFFFFFFFF };
	enum class PointLightID        : uint32_t { Invalid = 0xFFFFFFFF };
	enum class DirectionalLightID  : uint32_t { Invalid = 0xFFFFFFFF };
	enum class ModelAssetID        : uint32_t { Invalid = 0xFFFFFFFF };
	enum class ModelInstanceID     : uint32_t { Invalid = 0xFFFFFFFF };
	enum class SpotLightID         : uint32_t { Invalid = 0xFFFFFFFF };
	enum class RectAreaLightID     : uint32_t { Invalid = 0xFFFFFFFF };
	enum class DiscAreaLightID     : uint32_t { Invalid = 0xFFFFFFFF };

	template <typename T>
	[[nodiscard]] constexpr uint32_t ToIndex(T id) { return static_cast<uint32_t>(id); }
	template <typename T>
	[[nodiscard]] constexpr bool IsValid(T id) { return ToIndex(id) != 0xFFFFFFFFu; }

	struct alignas(16) Transform
	{
		Math::float4x4 worldMatrix;
	};
}

namespace dy::Graphics
{
	using ::dy::EntityID;
	using ::dy::MeshID;
	using ::dy::MaterialID;
	using ::dy::TextureID;
	using ::dy::PointLightID;
	using ::dy::DirectionalLightID;
	using ::dy::ModelAssetID;
	using ::dy::ModelInstanceID;
	using ::dy::SpotLightID;
	using ::dy::RectAreaLightID;
	using ::dy::DiscAreaLightID;
	using ::dy::Transform;
	using ::dy::ToIndex;
	using ::dy::IsValid;
}
