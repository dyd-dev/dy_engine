#pragma once

#include "Core/Image.h"

#include <string>

namespace dy::Core
{
	[[nodiscard]] Image LoadImageFromFile(const std::string& filepath);
}
