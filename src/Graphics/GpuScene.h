#pragma once
/* GpuScene
 *
 * Scene(CPU)의 텍스처를 GPU로 미러링하는 공유 리소스 캐시.
 * bind / bindless 렌더러가 공용으로 사용한다(텍스처 업로드·디스크립터 슬롯 단일 소유).
 * 메시/머티리얼의 GPU 표현은 렌더러 전략별로 다르므로 여기서 다루지 않는다.
 */
#include <cstdint>
#include <vector>

#include "Core/Types.h"
#include "RHI/IDevice.h"

namespace dy::RHI
{
	class IResourceSet;
	class ISampler;
	class ITexture;
}

namespace dy::Graphics
{
	class Scene;

	class GpuScene
	{
	public:
		// Scene의 텍스처를 GPU에 업로드하고, bindless set이 있으면 같은 texture index에 기록한다.
		void SyncTextures(const Scene& scene, RHI::IDevice* device, RHI::IResourceSet* bindlessSet, RHI::ISampler* sampler);
		// 보유한 GPU 텍스처를 해제한다.
		void Shutdown(RHI::IDevice* device);

		[[nodiscard]] RHI::ITexture* ResolveTexture(TextureID textureId) const;
		[[nodiscard]] uint32_t ResolveTextureIndex(TextureID textureId) const;
		[[nodiscard]] uint32_t GetTextureCount() const { return static_cast<uint32_t>(m_textures.size()); }

	private:
		struct TextureSlot
		{
			RHI::ITexture* texture = nullptr;
			bool bindlessAvailable = false;
		};

		std::vector<TextureSlot> m_textures;
	};
}
