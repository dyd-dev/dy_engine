#pragma once

#include <memory>

#include "Graphics/RendererDesc.h"

namespace dy::RHI { class IDevice; }

namespace dy::Graphics
{
	struct Camera;
	class Scene;
	class Canvas;
	struct TextureAsset;

	class Renderer
	{
	public:
		[[nodiscard]] static std::unique_ptr<Renderer> Create(
			const void* windowHandle,
			const RendererDesc& desc = {});
		// Takes ownership of an initialized RHI device without a swapchain.
		[[nodiscard]] static std::unique_ptr<Renderer> Create(
			std::unique_ptr<RHI::IDevice> device,
			const RendererDesc& desc = {});

		~Renderer();
		// false: no drawable is available yet; retry after processing window events.
		// Other failures throw. Optional readback requires RendererDesc::allowReadback.
		[[nodiscard]] bool Render(const Scene& scene, const Camera& camera, TextureAsset* readback = nullptr);
		[[nodiscard]] bool Render(const Canvas& canvas, TextureAsset* readback = nullptr);

	private:
		struct Impl;

		explicit Renderer(std::unique_ptr<Impl> impl);

		std::unique_ptr<Impl> m_impl;
	};
}
