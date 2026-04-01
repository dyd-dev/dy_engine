#pragma once
/* Texture
* 
* 1D/2D/3D Texture, Render Target, Depth Stencil 등을 할당합니다.
* Texture는 이미지 데이터를 GPU 메모리에 저장하고, 셰이더에서 이를 참조하여 렌더링에 활용할 수 있도록 합니다.
*/
#include <cstdint>
#include "Enums.h"

namespace dy::RHI
{
	// Descriptor for creating a texture
	struct TextureDesc {
		uint32_t width;
		uint32_t height;
		uint32_t depthOrArraySize = 1;
		uint32_t mipLevels = 1;
		Format format;
		TextureUsage usage;
	};

	class ITexture
	{
	public:
		virtual ~ITexture() = default;

		virtual uint32_t GetWidth() const = 0;
		virtual uint32_t GetHeight() const = 0;
		virtual Format GetFormat() const = 0;
	};
}