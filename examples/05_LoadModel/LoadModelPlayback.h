#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

#include "Core/Types.h"

namespace dy::Graphics
{
	class Scene;
}

namespace dy::Examples
{
	struct LoadModelOptions;

	struct LoadModelPlaybackResult
	{
		uint32_t animatedInstances = 0u;
		uint32_t staticInstances = 0u;
	};

	[[nodiscard]] bool ConfigureLoadModelAnimations(
		Graphics::Scene& scene,
		const std::vector<ModelInstanceID>& instances,
		const LoadModelOptions& options,
		std::ostream& output,
		std::string& outError,
		LoadModelPlaybackResult& outResult);
}
