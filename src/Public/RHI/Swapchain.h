#pragma once

#include <cstdint>

#include "Format.h"

namespace dy::RHI
{
	enum class PresentMode : uint32_t
	{
		Immediate,
		Mailbox,
		Fifo
	};

	struct SwapchainDesc
	{
		Format format = Format::Unknown;
		uint32_t minimumImageCount = 2;
		PresentMode presentMode = PresentMode::Fifo;
		bool allowReadback = false;
		// Optional initial framebuffer extent. Zero derives it from the window;
		// a nonzero extent must match the native window at creation. Null has no
		// window system and requires an explicit extent for command validation.
		uint32_t initialWidth = 0;
		uint32_t initialHeight = 0;
	};
}
