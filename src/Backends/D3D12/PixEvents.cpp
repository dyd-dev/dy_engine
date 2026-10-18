#include "PixEvents.h"

#if defined(DY_RENDERDOC_ENABLED)
#include "dyf/RHI/ICommandList.h"
#include <algorithm>
#include <d3d12.h>
#include <pix3.h>
#endif

namespace dyf::Backends
{
	void BeginPixEvent(void* commandList, const char* name, const RHI::DebugLabelColor& color) noexcept
	{
#if defined(DY_RENDERDOC_ENABLED)
		const auto channel = [](float value) { return UINT8(std::clamp(value, 0.f, 1.f) * 255); };
		PIXBeginEvent(static_cast<ID3D12GraphicsCommandList*>(commandList),
			PIX_COLOR(channel(color.r), channel(color.g), channel(color.b)), "%s", name);
#else
		(void)commandList;
		(void)name;
		(void)color;
#endif
	}

	void EndPixEvent(void* commandList) noexcept
	{
#if defined(DY_RENDERDOC_ENABLED)
		PIXEndEvent(static_cast<ID3D12GraphicsCommandList*>(commandList));
#else
		(void)commandList;
#endif
	}

	void InsertPixMarker(void* commandList, const char* name, const RHI::DebugLabelColor& color) noexcept
	{
#if defined(DY_RENDERDOC_ENABLED)
		const auto channel = [](float value) { return UINT8(std::clamp(value, 0.f, 1.f) * 255); };
		PIXSetMarker(static_cast<ID3D12GraphicsCommandList*>(commandList),
			PIX_COLOR(channel(color.r), channel(color.g), channel(color.b)), "%s", name);
#else
		(void)commandList;
		(void)name;
		(void)color;
#endif
	}
}
