#pragma once

#include <cstdint>

namespace dy::RHI
{
	class ITexture;

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
		Load,
		Clear,
		DontCare
	};

	enum class StoreOp : uint8_t
	{
		Store,
		DontCare
	};

	struct ColorAttachmentInfo
	{
		ITexture* texture = nullptr;
		LoadOp loadOp = LoadOp::Load;
		StoreOp storeOp = StoreOp::Store;
		float clearColor[4] = {};
	};

	struct DepthStencilAttachmentInfo
	{
		ITexture* texture = nullptr;
		LoadOp depthLoadOp = LoadOp::Load;
		StoreOp depthStoreOp = StoreOp::Store;
		float clearDepth = 1.0f;
		LoadOp stencilLoadOp = LoadOp::Load;
		StoreOp stencilStoreOp = StoreOp::Store;
		uint32_t clearStencil = 0;
	};

	struct RenderingInfo
	{
		const ColorAttachmentInfo* colorAttachments = nullptr;
		uint32_t colorAttachmentCount = 0;
		const DepthStencilAttachmentInfo* depthStencilAttachment = nullptr;
	};
}
