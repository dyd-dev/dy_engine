#pragma once

#include <cstdint>

#include "Format.h"

namespace dyf::RHI
{
	enum class PresentMode : uint32_t
	{
		Immediate,
		Mailbox,
		Fifo
	};

    // 출력 장치에 전달할 색 공간과 창 합성 방식이다. 미지원 요청은 생성에 실패한다.
    enum class ColorSpace : uint8_t { Srgb, LinearSrgb };
    enum class CompositeAlpha : uint8_t { Opaque, Premultiplied, Postmultiplied, Inherit };

	struct SwapchainDesc
	{
		Format format = Format::Unknown;
		// 정확한 개수가 아닌 최소 개수. 실제 개수가 더 크면 Info 진단에 요청/실제 값을 전달한다.
		// 네이티브 최대치를 넘는 요청은 줄여서 적용하지 않고 생성에 실패한다.
		uint32_t minimumImageCount = 2;
		PresentMode presentMode = PresentMode::Fifo;
		bool allowReadback = false;
		// Optional initial framebuffer extent. Zero derives it from the window;
		// a nonzero extent must match the native window at creation. Null has no
		// window system and requires an explicit extent for command validation.
		uint32_t initialWidth = 0;
		uint32_t initialHeight = 0;
        const void* window = nullptr;
        ColorSpace colorSpace = ColorSpace::Srgb;
        CompositeAlpha compositeAlpha = CompositeAlpha::Opaque;
	};
}
