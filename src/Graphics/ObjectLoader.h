#pragma once

#include "Core/Types.h"
#include "Graphics/MaterialTexturePaths.h"
#include "Math/Math.h"

#include <string>

namespace dy::Graphics
{
	class Scene;

	struct ObjectLoadResult
	{
		bool succeeded = false;
		EntityID entity = EntityID::Invalid;
		MeshID mesh = MeshID::Invalid;
		MaterialID material = MaterialID::Invalid;
		std::string resolvedPath;
		std::string texturePath;
		MaterialTexturePaths texturePaths;

		[[nodiscard]] explicit operator bool() const { return succeeded; }
	};

	class ObjectLoader
	{
	public:
		static ObjectLoadResult Load(
			Scene& scene,
			const std::string& filepath,
			const Material& material = Material{},
			const Math::float4x4& worldMatrix = Math::float4x4::Identity(),
			const RenderFlags& renderFlags = {});

		static ObjectLoadResult LoadOBJ(
			Scene& scene,
			const std::string& filepath,
			const Material& material = Material{},
			const Math::float4x4& worldMatrix = Math::float4x4::Identity(),
			const RenderFlags& renderFlags = {});

		static ObjectLoadResult LoadFBX(
			Scene& scene,
			const std::string& filepath,
			const Material& material = Material{},
			const Math::float4x4& worldMatrix = Math::float4x4::Identity(),
			const RenderFlags& renderFlags = {});
	};
}
