#pragma once

#include <cstdint>

#include "ResourceHandles.h"
#include "ResourceState.h"

namespace dy::RHI
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
		LoadOp stencilLoadOp = LoadOp::Undefined;
		StoreOp stencilStoreOp = StoreOp::Undefined;
		uint32_t clearStencil = 0;
	};

	struct RenderingDesc
	{
		const ColorAttachment* colorAttachments = nullptr;
		uint32_t colorAttachmentCount = 0;
		const DepthStencilAttachment* depthStencilAttachment = nullptr;
	};
}
