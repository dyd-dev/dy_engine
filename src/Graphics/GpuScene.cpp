#include "Graphics/GpuScene.h"

#include "Graphics/ImageFile.h"
#include "Graphics/Scene.h"
#include "RHI/IDevice.h"
#include "RHI/IResourceSet.h"
#include "RHI/ISampler.h"
#include "RHI/ITexture.h"

namespace dy::Graphics
{
	namespace
	{
		struct UploadImageView
		{
			uint32_t width = 0;
			uint32_t height = 0;
			uint32_t rowPitch = 0;
			const uint8_t* pixels = nullptr;
		};

		[[nodiscard]] UploadImageView ResolveTextureUpload(const TextureAsset& textureData, ImageFile& loadedImage)
		{
			if(!textureData.rgba8.empty() && textureData.width > 0u && textureData.height > 0u)
			{
				return UploadImageView{
					textureData.width,
					textureData.height,
					textureData.width * 4u,
					textureData.rgba8.data()
				};
			}

			loadedImage = LoadImageFile(textureData.sourcePath);
			if(!loadedImage.IsValid()) return {};
			return UploadImageView{
				loadedImage.GetWidth(),
				loadedImage.GetHeight(),
				loadedImage.GetRowPitch(),
				loadedImage.GetPixels().data()
			};
		}
	}

	void GpuScene::SyncTextures(const Scene& scene, RHI::IDevice* device, RHI::IResourceSet* bindlessSet, RHI::ISampler* sampler)
	{
		if(device == nullptr) return;

		const uint32_t textureCount = scene.GetTextureCount();
		if(m_textures.size() < textureCount) m_textures.resize(textureCount);

		for(uint32_t textureIndex = 0; textureIndex < textureCount; ++textureIndex)
		{
			TextureSlot& slot = m_textures[textureIndex];
			if(slot.texture != nullptr)
			{
				if(bindlessSet != nullptr && sampler != nullptr && !slot.bindlessAvailable)
				{
					const RHI::ResourceSetWrite write = { 1, 0, textureIndex, nullptr, slot.texture, sampler, 0, 0 };
					slot.bindlessAvailable = device->UpdateResourceSet(bindlessSet, &write, 1);
				}
				continue;
			}

			const TextureAsset& textureData = scene.GetTexture(static_cast<TextureID>(textureIndex));
			ImageFile loadedImage;
			const UploadImageView upload = ResolveTextureUpload(textureData, loadedImage);
			if(upload.pixels == nullptr) continue;

			RHI::TextureDesc textureDesc = {};
			textureDesc.width = upload.width;
			textureDesc.height = upload.height;
			textureDesc.depthOrArraySize = 1;
			textureDesc.mipLevels = 1;
			textureDesc.format = RHI::Format::R8G8B8A8_UNORM;
			textureDesc.usage = RHI::TextureUsage::ShaderResource;

			slot.texture = device->CreateTexture(textureDesc);
			if(slot.texture != nullptr)
			{
				if(device->UpdateTexture(slot.texture, upload.pixels, upload.rowPitch))
				{
					if(bindlessSet != nullptr && sampler != nullptr)
					{
						const RHI::ResourceSetWrite write = { 1, 0, textureIndex, nullptr, slot.texture, sampler, 0, 0 };
						slot.bindlessAvailable = device->UpdateResourceSet(bindlessSet, &write, 1);
					}
				}
				else
				{
					device->DestroyTexture(slot.texture);
					slot.texture = nullptr;
					slot.bindlessAvailable = false;
				}
			}
		}
	}

	void GpuScene::Shutdown(RHI::IDevice* device)
	{
		if(device == nullptr) return;
		for(TextureSlot& slot : m_textures)
		{
			if(slot.texture != nullptr)
			{
				device->DestroyTexture(slot.texture);
				slot.texture = nullptr;
			}
		}
		m_textures.clear();
	}

	RHI::ITexture* GpuScene::ResolveTexture(TextureID textureId) const
	{
		if(!IsValid(textureId)) return nullptr;
		const uint32_t textureIndex = ToIndex(textureId);
		if(textureIndex >= m_textures.size()) return nullptr;
		return m_textures[textureIndex].texture;
	}

	uint32_t GpuScene::ResolveTextureIndex(TextureID textureId) const
	{
		if(!IsValid(textureId)) return 0xFFFFFFFFu;
		const uint32_t textureIndex = ToIndex(textureId);
		if(textureIndex >= m_textures.size() || !m_textures[textureIndex].bindlessAvailable) return 0xFFFFFFFFu;
		return textureIndex;
	}
}
