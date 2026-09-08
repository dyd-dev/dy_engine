#pragma once

#include <cstdint>
#include <string>

namespace dy::Examples
{
	struct LoadModelOptions
	{
		std::string modelPath;
		uint32_t clipIndex = 0u;
		float timeScale = 1.0f;
		float smokeSeconds = 0.0f;
		bool paused = false;
		bool loop = true;
	};

	[[nodiscard]] bool ParseLoadModelOptions(
		int argumentCount,
		const char* const* arguments,
		LoadModelOptions& outOptions,
		std::string& outError);
}
