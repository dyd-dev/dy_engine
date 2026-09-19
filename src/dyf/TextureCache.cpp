#include "dyf/Image.h"
#include "dyf/Renderer.h"
#include "dyf/Scene.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/Texture.h"

#include <limits>
#include <vector>

namespace dyf
{
	// 공개 재질의 이미지 입력으로 RHI 캐시를 갱신한다. 같은 픽셀과 색 공간은 한 번 업로드한다.
	bool Renderer::SyncTextures(const Scene& scene, RHI::IDevice* device)
	{
		if(device == nullptr) return false;

		m_indices.clear();
		std::vector<const Image*> images;
		for(const auto& material : scene.Materials())
		{
			for(const Image* image : {&material.baseColorTexture, &material.metallicRoughnessTexture,
				&material.normalTexture, &material.occlusionTexture, &material.emissiveTexture})
			{
				if(image->IsValid() && m_indices.emplace(
					std::make_pair(image->GetPixels().data(), image->GetColorSpace()),
					static_cast<uint32_t>(images.size())).second)
				{
					images.push_back(image);
				}
			}
		}

		while(m_textures.size() > images.size())
		{
			if(m_textures.back().texture) device->DestroyTexture(m_textures.back().texture);
			m_textures.pop_back();
		}
		m_textures.resize(images.size());

		bool uploadFailed = false;
		RHI::ICommandList* commandList = nullptr;
		// 기록한 상태는 제출이 성공한 뒤에만 캐시에 반영한다.
		std::vector<RHI::ResourceState> submittedStates(images.size(), RHI::ResourceState::Undefined);
		for(uint32_t textureIndex = 0; textureIndex < images.size(); ++textureIndex)
		{
			const Image& image = *images[textureIndex];
			TextureSlot& slot = m_textures[textureIndex];
			if(slot.source.GetPixels().data() != image.GetPixels().data() ||
				slot.source.GetColorSpace() != image.GetColorSpace())
			{
				if(slot.texture) device->DestroyTexture(slot.texture);
				slot = {};
				slot.source = image;
			}
			if(slot.state == RHI::ResourceState::ShaderResource) continue;

			const auto& pixels = image.GetPixels();
			if(pixels.size() > std::numeric_limits<uint32_t>::max())
			{
				uploadFailed = true;
				continue;
			}
			const uint32_t uploadSize = static_cast<uint32_t>(pixels.size());
			const uint32_t rowPitch = image.GetWidth() * 4;

			RHI::TextureDesc textureDesc;
			textureDesc.width = image.GetWidth();
			textureDesc.height = image.GetHeight();
			textureDesc.depthOrArraySize = textureDesc.mipLevels = 1;
			textureDesc.format = image.GetColorSpace() == ColorSpace::Srgb
				? RHI::Format::R8G8B8A8_UNORM_SRGB : RHI::Format::R8G8B8A8_UNORM;
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

			RHI::ResourceBarrierDesc barrier = {
				nullptr, slot.texture, slot.state, RHI::ResourceState::CopyDestination, {}
			};
			commandList->ResourceBarrier(&barrier, 1);
			const bool uploaded = device->UpdateTexture(
				*commandList, slot.texture, 0, 0, pixels.data(), uploadSize, rowPitch, uploadSize);
			uploadFailed = uploadFailed || !uploaded;
			barrier.before = RHI::ResourceState::CopyDestination;
			barrier.after = uploaded ? RHI::ResourceState::ShaderResource : RHI::ResourceState::Common;
			commandList->ResourceBarrier(&barrier, 1);
			submittedStates[textureIndex] = barrier.after;
		}

		if(commandList == nullptr) return !uploadFailed;
		const bool submitted = commandList->Close() && device->Submit(&commandList, 1);
		device->DestroyCommandList(commandList);
		if(!submitted) return false;

		for(uint32_t textureIndex = 0; textureIndex < submittedStates.size(); ++textureIndex)
		{
			if(submittedStates[textureIndex] != RHI::ResourceState::Undefined)
				m_textures[textureIndex].state = submittedStates[textureIndex];
		}
		return !uploadFailed;
	}

	void Renderer::ReleaseTextures(RHI::IDevice* device)
	{
		if(device == nullptr) return;
		for(TextureSlot& slot : m_textures)
		{
			if(slot.texture) device->DestroyTexture(slot.texture);
		}
		m_textures.clear();
		m_indices.clear();
	}

	RHI::TextureHandle Renderer::ResolveTexture(const Image& image) const
	{
		const auto found = m_indices.find({image.GetPixels().data(), image.GetColorSpace()});
		if(found == m_indices.end()) return nullptr;
		const TextureSlot& slot = m_textures[found->second];
		return slot.state == RHI::ResourceState::ShaderResource ? slot.texture : nullptr;
	}
}
