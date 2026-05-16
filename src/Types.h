#pragma once
#include <cstdint>

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
}
