#pragma once

#include <cstdint>

namespace dy::RHI::RendererDefaults
{
	constexpr uint32_t kMaxFramesInFlight = 2;
	constexpr uint32_t kMaxDrawsPerFrame = 128;
	constexpr uint32_t kMaxRenderTargets = 4;
	constexpr uint32_t kShadowMapResolution = 2048;
	constexpr uint32_t kFallbackTextureWidth = 2;
	constexpr uint32_t kFallbackTextureHeight = 2;
	constexpr uint64_t kFrameAcquireTimeoutNanoseconds = 16666667ull;
}
