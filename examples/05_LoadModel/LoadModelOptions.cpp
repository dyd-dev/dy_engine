#include "LoadModelOptions.h"

#include <charconv>
#include <cmath>
#include <string_view>

namespace
{
	bool ParseUnsigned(std::string_view text, uint32_t& outValue)
	{
		if(text.empty()) return false;
		const char* begin = text.data();
		const char* end = begin + text.size();
		const std::from_chars_result result = std::from_chars(begin, end, outValue);
		return result.ec == std::errc{} && result.ptr == end;
	}

	bool ParseFiniteFloat(std::string_view text, float& outValue)
	{
		if(text.empty()) return false;
		const char* begin = text.data();
		const char* end = begin + text.size();
		const std::from_chars_result result = std::from_chars(begin, end, outValue, std::chars_format::general);
		return result.ec == std::errc{} && result.ptr == end && std::isfinite(outValue);
	}
}

namespace dy::Examples
{
	bool ParseLoadModelOptions(
		int argumentCount,
		const char* const* arguments,
		LoadModelOptions& outOptions,
		std::string& outError)
	{
		outOptions = {};
		outError.clear();
		if(argumentCount < 0 || (argumentCount > 0 && arguments == nullptr))
		{
			outError = "invalid argument array";
			return false;
		}

		for(int argumentIndex = 1; argumentIndex < argumentCount; ++argumentIndex)
		{
			const std::string_view argument = arguments[argumentIndex] != nullptr
				? std::string_view(arguments[argumentIndex])
				: std::string_view{};
			if(argument == "--paused")
			{
				outOptions.paused = true;
				continue;
			}

			constexpr std::string_view clipPrefix = "--clip=";
			if(argument.rfind(clipPrefix, 0u) == 0u)
			{
				if(!ParseUnsigned(argument.substr(clipPrefix.size()), outOptions.clipIndex))
				{
					outError = "--clip requires a non-negative integer";
					return false;
				}
				continue;
			}

			constexpr std::string_view timeScalePrefix = "--timescale=";
			if(argument.rfind(timeScalePrefix, 0u) == 0u)
			{
				if(!ParseFiniteFloat(argument.substr(timeScalePrefix.size()), outOptions.timeScale))
				{
					outError = "--timescale requires a finite number";
					return false;
				}
				continue;
			}

			constexpr std::string_view loopPrefix = "--loop=";
			if(argument.rfind(loopPrefix, 0u) == 0u)
			{
				const std::string_view value = argument.substr(loopPrefix.size());
				if(value != "0" && value != "1")
				{
					outError = "--loop requires 0 or 1";
					return false;
				}
				outOptions.loop = value == "1";
				continue;
			}

			constexpr std::string_view smokePrefix = "--smoke-seconds=";
			if(argument.rfind(smokePrefix, 0u) == 0u)
			{
				if(!ParseFiniteFloat(argument.substr(smokePrefix.size()), outOptions.smokeSeconds)
					|| outOptions.smokeSeconds < 0.0f)
				{
					outError = "--smoke-seconds requires a finite non-negative number";
					return false;
				}
				continue;
			}

			if(argument.empty() || argument.rfind("--", 0u) == 0u)
			{
				outError = "unknown option: " + std::string(argument);
				return false;
			}
			if(!outOptions.modelPath.empty())
			{
				outError = "only one model path may be specified";
				return false;
			}
			outOptions.modelPath = argument;
		}
		return true;
	}
}
