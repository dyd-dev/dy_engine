#pragma once
#include <cstdint>

#include "dyf/Math/Math.h"

namespace dyf
{
    struct Rectangle { float x = 0, y = 0, width = 0, height = 0; };

	enum class EntityID            : uint32_t { Invalid = 0xFFFFFFFF };
	enum class MeshID              : uint32_t { Invalid = 0xFFFFFFFF };
	enum class MaterialID          : uint32_t { Invalid = 0xFFFFFFFF };
	enum class TextureID           : uint32_t { Invalid = 0xFFFFFFFF };
	enum class PointLightID        : uint32_t { Invalid = 0xFFFFFFFF };
	enum class DirectionalLightID  : uint32_t { Invalid = 0xFFFFFFFF };
	enum class SpotLightID         : uint32_t { Invalid = 0xFFFFFFFF };
	enum class RectAreaLightID     : uint32_t { Invalid = 0xFFFFFFFF };
	enum class DiscAreaLightID     : uint32_t { Invalid = 0xFFFFFFFF };

	template <typename T>
	[[nodiscard]] constexpr uint32_t ToIndex(T id) { return static_cast<uint32_t>(id); }
	template <typename T>
	[[nodiscard]] constexpr bool IsValid(T id) { return ToIndex(id) != 0xFFFFFFFFu; }

	struct alignas(16) Transform
	{
		Math::float4x4 worldMatrix = Math::float4x4::Identity();
	};
}
