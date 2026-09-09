#include "Graphics/Private/TextureCache.h"

#include "Graphics/Scene.h"
#include "RHI/ICommandList.h"
#include "RHI/IDevice.h"
#include "RHI/Texture.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#define STBI_NO_STDIO_DEPRECATED
#include "stb_image.h"

namespace dy::Graphics::Private
{
	namespace
	{
		struct DecodedImage
		{
			uint32_t width = 0;
			uint32_t height = 0;
			std::vector<uint8_t> pixels;
		};

		struct UploadImageView
		{
			uint32_t width = 0;
			uint32_t height = 0;
			uint32_t rowPitch = 0;
			std::size_t size = 0;
			const uint8_t* pixels = nullptr;
		};

		[[nodiscard]] DecodedImage DecodeRgba8(const std::string& path)
		{
			DecodedImage image;
			if(path.empty()) return image;

			int width = 0;
			int height = 0;
			int channels = 0;
			unsigned char* data = stbi_load(path.c_str(), &width, &height, &channels, 4);
			if(data == nullptr || width <= 0 || height <= 0)
			{
				if(data != nullptr) stbi_image_free(data);
				return image;
			}

			image.width = static_cast<uint32_t>(width);
			image.height = static_cast<uint32_t>(height);
			image.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);
			std::memcpy(image.pixels.data(), data, image.pixels.size());
			stbi_image_free(data);
			return image;
		}

		[[nodiscard]] UploadImageView ResolveTextureUpload(
			const TextureAsset& textureData,
			DecodedImage& decodedImage)
		{
			const uint64_t embeddedSize = static_cast<uint64_t>(textureData.width) * textureData.height * 4u;
			if(textureData.width > 0u && textureData.height > 0u &&
				textureData.width <= std::numeric_limits<uint32_t>::max() / 4u &&
				embeddedSize <= textureData.rgba8.size())
			{
				return UploadImageView{
					textureData.width,
					textureData.height,
					textureData.width * 4u,
					textureData.rgba8.size(),
					textureData.rgba8.data()
				};
			}

			decodedImage = DecodeRgba8(textureData.sourcePath);
			if(decodedImage.pixels.empty() ||
				decodedImage.width == 0u ||
				decodedImage.height == 0u ||
				decodedImage.width > std::numeric_limits<uint32_t>::max() / 4u)
			{
				return {};
			}
			return UploadImageView{
				decodedImage.width,
				decodedImage.height,
				decodedImage.width * 4u,
				decodedImage.pixels.size(),
				decodedImage.pixels.data()
			};
		}
	}

	bool TextureCache::Sync(const Scene& scene, RHI::IDevice* device)
	{
		if(device == nullptr) return false;
		struct SubmittedState
		{
			TextureSlot* slot = nullptr;
			RHI::ResourceState state = RHI::ResourceState::Undefined;
		};

		const uint32_t textureCount = scene.GetTextureCount();
		if(m_textures.size() < textureCount) m_textures.resize(textureCount);
		bool uploadFailed = false;
		RHI::ICommandList* commandList = nullptr;
		std::vector<SubmittedState> submittedStates;

		for(uint32_t textureIndex = 0; textureIndex < textureCount; ++textureIndex)
		{
			TextureSlot& slot = m_textures[textureIndex];
			if(slot.state == RHI::ResourceState::ShaderResource) continue;

			const TextureAsset& textureData = scene.GetTexture(static_cast<TextureID>(textureIndex));
			DecodedImage decodedImage;
			const UploadImageView upload = ResolveTextureUpload(textureData, decodedImage);
			if(upload.pixels == nullptr) continue;
			const uint64_t uploadSize = static_cast<uint64_t>(upload.rowPitch) * upload.height;
			if(uploadSize > upload.size || uploadSize > std::numeric_limits<uint32_t>::max())
			{
				uploadFailed = true;
				continue;
			}

			RHI::TextureDesc textureDesc = {};
			textureDesc.width = upload.width;
			textureDesc.height = upload.height;
			textureDesc.depthOrArraySize = 1;
			textureDesc.mipLevels = 1;
			textureDesc.format = RHI::Format::R8G8B8A8_UNORM;
			textureDesc.usage = RHI::TextureUsage::ShaderResource;

			const bool created = slot.texture == nullptr;
			if(created) slot.texture = device->CreateTexture(textureDesc);
			if(slot.texture == nullptr)
			{
				uploadFailed = true;
				continue;
			}
			if(commandList == nullptr) commandList = device->AcquireCommandList();
			if(commandList == nullptr)
			{
				uploadFailed = true;
				if(created)
				{
					device->DestroyTexture(slot.texture);
					slot.texture = nullptr;
				}
				continue;
			}

			const RHI::ResourceBarrierDesc beforeCopy = {
				nullptr,
				slot.texture,
				slot.state,
				RHI::ResourceState::CopyDestination,
				{}
			};
			commandList->ResourceBarrier(&beforeCopy, 1);
			if(!device->UpdateTexture(
				*commandList,
				slot.texture,
				0,
				0,
				upload.pixels,
				static_cast<uint32_t>(uploadSize),
				upload.rowPitch,
				static_cast<uint32_t>(uploadSize)))
			{
				uploadFailed = true;
				const RHI::ResourceBarrierDesc restore = {
					nullptr,
					slot.texture,
					RHI::ResourceState::CopyDestination,
					RHI::ResourceState::Common,
					{}
				};
				commandList->ResourceBarrier(&restore, 1);
				submittedStates.push_back({ &slot, RHI::ResourceState::Common });
				continue;
			}

			const RHI::ResourceBarrierDesc barrier = {
				nullptr,
				slot.texture,
				RHI::ResourceState::CopyDestination,
				RHI::ResourceState::ShaderResource,
				{}
			};
			commandList->ResourceBarrier(&barrier, 1);
			submittedStates.push_back({ &slot, RHI::ResourceState::ShaderResource });
		}

		if(commandList == nullptr) return !uploadFailed;
		commandList->Close();
		std::array<RHI::ICommandList*, 1> upload = { commandList };
		if(!device->Submit(upload.data(), 1)) return false;
		for(const SubmittedState& submitted : submittedStates)
		{
			submitted.slot->state = submitted.state;
		}
		return !uploadFailed;
	}

	void TextureCache::Shutdown(RHI::IDevice* device)
	{
		if(device == nullptr) return;
		for(TextureSlot& slot : m_textures)
		{
			if(slot.texture != nullptr)
			{
				device->DestroyTexture(slot.texture);
				slot.texture = nullptr;
				slot.state = RHI::ResourceState::Undefined;
			}
		}
		m_textures.clear();
	}

	RHI::TextureHandle TextureCache::Resolve(TextureID textureId) const
	{
		if(!IsValid(textureId)) return nullptr;
		const uint32_t textureIndex = ToIndex(textureId);
		if(textureIndex >= m_textures.size()) return nullptr;
		const TextureSlot& slot = m_textures[textureIndex];
		return slot.state == RHI::ResourceState::ShaderResource ? slot.texture : nullptr;
	}
}
