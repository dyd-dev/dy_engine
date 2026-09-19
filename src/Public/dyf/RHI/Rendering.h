#pragma once

#include <cstdint>

#include "ResourceHandles.h"
#include "ResourceState.h"

namespace dyf::RHI
{
	struct Viewport
	{
		float x = 0.0f;
		float y = 0.0f;
		float width = 0.0f;
		float height = 0.0f;
		float minDepth = 0.0f;
		float maxDepth = 1.0f;
	};

	struct Rect
	{
		int32_t x = 0;
		int32_t y = 0;
		uint32_t width = 0;
		uint32_t height = 0;
	};

	enum class LoadOp : uint8_t
	{
		Undefined,
		Load,
		Clear,
		Discard
	};

	enum class StoreOp : uint8_t
	{
		Undefined,
		Store,
		Discard
	};

	struct ColorAttachment
	{
		TextureHandle texture = nullptr;
		uint32_t mipLevel = 0;
		uint32_t arrayLayer = 0;
		LoadOp loadOp = LoadOp::Undefined;
		StoreOp storeOp = StoreOp::Undefined;
		// loadOp=Clear일 때만 사용한다. R16_UINT/R32_UINT는 [0]을 해당 범위의
		// 음이 아닌 정수로 지정한다(float로 정확히 표현할 수 있는 값). 반올림/비트 재해석하지 않는다.
		// 포맷에 없는 채널은 사용하지 않는다. UNORM의 [0,1] 변환은 포맷 자체의 저장 규칙이다.
		float clearColor[4] = {};
	};

	struct DepthStencilAttachment
	{
		TextureHandle texture = nullptr;
		uint32_t mipLevel = 0;
		uint32_t arrayLayer = 0;
		ResourceState state = ResourceState::Undefined;
		LoadOp depthLoadOp = LoadOp::Undefined;
		StoreOp depthStoreOp = StoreOp::Undefined;
		float clearDepth = 1.0f;
		// D32_FLOAT에는 stencil 성분이 없다. 설정한 stencil load/store/clear는 stderr로 알리고 무시한다.
		LoadOp stencilLoadOp = LoadOp::Undefined;
		StoreOp stencilStoreOp = StoreOp::Undefined;
		// D24_UNORM_S8_UINT의 stencilLoadOp=Clear일 때 0..255. 범위 초과를 자르지 않는다.
		uint32_t clearStencil = 0;
	};

	struct RenderingDesc
	{
		const ColorAttachment* colorAttachments = nullptr;
		uint32_t colorAttachmentCount = 0;
		const DepthStencilAttachment* depthStencilAttachment = nullptr;
	};
}
