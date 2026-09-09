#pragma once

#include <cstdint>
#include <vector>

#include "Graphics/Texture.h"
#include "RHI/ResourceHandles.h"
#include "RHI/ResourceState.h"

namespace dy::RHI
{
	class IDevice;
}

namespace dy::Graphics
{
	class Scene;
}

namespace dy::Graphics::Private
{
	class TextureCache
	{
	public:
		[[nodiscard]] bool Sync(const Scene& scene, RHI::IDevice* device);
		void Shutdown(RHI::IDevice* device);

		[[nodiscard]] RHI::TextureHandle Resolve(TextureID textureId) const;

	private:
		struct TextureSlot
		{
			RHI::TextureHandle texture = nullptr;
			RHI::ResourceState state = RHI::ResourceState::Undefined;
		};

		std::vector<TextureSlot> m_textures;
	};
}
