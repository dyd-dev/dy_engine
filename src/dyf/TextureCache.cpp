#include "dyf/Image.h"
#include "RendererInternal.h"
#include "dyf/Scene.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/Texture.h"
#include "../RHI/ResourceScope.h"

#define STBIRDEF static inline
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>
#undef STBIRDEF

#include <algorithm>
#include <array>
#include <cstdio>
#include <deque>
#include <future>
#include <limits>
#include <vector>

namespace dyf
{
	// 공개 재질의 이미지 입력으로 RHI 캐시를 갱신한다. 같은 픽셀과 색 공간은 한 번 업로드한다.
	bool Renderer::Impl::SyncTextures(const Scene& scene, RHI::IDevice* device)
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

		std::vector<uint32_t> pending;
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
			if(image.GetPixels().size() > std::numeric_limits<uint32_t>::max() ||
				image.GetWidth() > static_cast<uint32_t>(std::numeric_limits<int>::max() / 4) ||
				image.GetHeight() > static_cast<uint32_t>(std::numeric_limits<int>::max()))
			{
				std::fprintf(stderr, "dyf: texture %u/%zu exceeds the upload or mipmap size limit.\n",
					textureIndex + 1, images.size());
				return false;
			}
			pending.push_back(textureIndex);
		}
		if(pending.empty()) return true;

		struct PreparedMips
		{
			std::vector<std::vector<uint8_t>> levels;
			bool valid = true;
		};
		const auto prepareMips = [&](uint32_t textureIndex) {
			const Image& image = *images[textureIndex];
			PreparedMips result;
			uint32_t width = image.GetWidth(), height = image.GetHeight();
			const uint8_t* source = image.GetPixels().data();
			const bool srgb = image.GetColorSpace() == ColorSpace::Srgb;
			while(width > 1 || height > 1)
			{
				const uint32_t nextWidth = std::max(1u, width / 2);
				const uint32_t nextHeight = std::max(1u, height / 2);
				std::vector<uint8_t> next(static_cast<size_t>(nextWidth) * nextHeight * 4);
				// Preserve linear-light color filtering and independent data channels.
				auto* resized = srgb
					? stbir_resize_uint8_srgb(source, static_cast<int>(width), static_cast<int>(height), 0,
						next.data(), static_cast<int>(nextWidth), static_cast<int>(nextHeight), 0, STBIR_RGBA)
					: stbir_resize_uint8_linear(source, static_cast<int>(width), static_cast<int>(height), 0,
						next.data(), static_cast<int>(nextWidth), static_cast<int>(nextHeight), 0, STBIR_4CHANNEL);
				if(!resized) { result.valid = false; break; }
				result.levels.push_back(std::move(next));
				source = result.levels.back().data();
				width = nextWidth;
				height = nextHeight;
			}
			return result;
		};
		const size_t preparationCount = std::min<size_t>(config.texturePreparationThreadCount, pending.size());
		const auto launch = preparationCount == 1 ? std::launch::deferred : std::launch::async;
		// Futures are destroyed before their referenced input. On any failure the
		// remaining temporary CPU jobs finish before SyncTextures returns.
		std::deque<std::future<PreparedMips>> prepared;
		size_t nextPreparation = 0;
		for(; nextPreparation < preparationCount; ++nextPreparation)
			prepared.emplace_back(std::async(launch, prepareMips, pending[nextPreparation]));

		constexpr uint64_t uploadTimeoutNanoseconds = 5000000000ull;
		for(uint32_t textureIndex : pending)
		{
			const Image& image = *images[textureIndex];
			const auto fail = [&](const char* reason) {
				std::fprintf(stderr, "dyf: texture upload %u/%zu (%u x %u): %s\n",
					textureIndex + 1, images.size(), image.GetWidth(), image.GetHeight(), reason);
				return false;
			};
			TextureSlot& slot = m_textures[textureIndex];

			RHI::TextureDesc textureDesc;
			textureDesc.width = image.GetWidth();
			textureDesc.height = image.GetHeight();
			for(uint32_t dimension = std::max(textureDesc.width, textureDesc.height); dimension > 1; dimension >>= 1)
				++textureDesc.mipLevels;
			const bool srgb = image.GetColorSpace() == ColorSpace::Srgb;
			textureDesc.format = srgb
				? RHI::Format::R8G8B8A8_UNORM_SRGB : RHI::Format::R8G8B8A8_UNORM;
			textureDesc.usage = RHI::TextureUsage::ShaderResource;

			if(slot.texture == nullptr) slot.texture = device->CreateTexture(textureDesc);
			if(slot.texture == nullptr)
				return fail("GPU texture allocation failed.");

			RHI::FenceHandle completion;
			{
				PreparedMips mips = prepared.front().get();
				prepared.pop_front();
				if(!mips.valid) return fail("mipmap generation failed.");
				// Keep only one image's recorded bytes and native staging allocation
				// alive at once, rather than duplicating the entire decoded scene.
				RHI::ResourceScope resources(*device);
				auto* commandList = device->AcquireCommandList();
				if(commandList == nullptr) return fail("command list allocation failed.");
				resources.Keep(commandList);

				RHI::ResourceBarrierDesc barrier = {
					nullptr, slot.texture, slot.state, RHI::ResourceState::CopyDestination, {}
				};
				commandList->ResourceBarrier(&barrier, 1);
				uint32_t width = textureDesc.width, height = textureDesc.height;
				for(uint32_t mip = 0; mip < textureDesc.mipLevels; ++mip)
				{
					const uint8_t* mipData = mip == 0 ? image.GetPixels().data() : mips.levels[mip - 1].data();
					const uint32_t rowPitch = width * 4;
					const uint32_t uploadSize = rowPitch * height;
					if(!device->UpdateTexture(*commandList, slot.texture, mip, 0,
						mipData, uploadSize, rowPitch, uploadSize))
						return fail("pixel upload recording failed.");
					width = std::max(1u, width / 2);
					height = std::max(1u, height / 2);
				}
				barrier.before = RHI::ResourceState::CopyDestination;
				barrier.after = RHI::ResourceState::ShaderResource;
				commandList->ResourceBarrier(&barrier, 1);
				if(!commandList->Close() || !device->Submit({&commandList, 1, nullptr, 0}, completion))
					return fail("GPU submission failed.");

				// Submission commits the queued resource state, even if the bounded
				// completion wait below fails. Native commands retain in-flight data.
				slot.state = barrier.after;
			}
			// Release the recorded CPU copy before waiting. Retiring this submission
			// also releases its native staging pages before the next image is copied.
			if(!device->Wait(completion, uploadTimeoutNanoseconds))
				return fail(device->IsLost() ? "device became unavailable while waiting for the upload."
					: "GPU upload did not complete within five seconds.");
			if(nextPreparation < pending.size())
				prepared.emplace_back(std::async(launch, prepareMips, pending[nextPreparation++]));
		}
		return true;
	}

	void Renderer::Impl::ReleaseTextures(RHI::IDevice* device)
	{
		for(TextureSlot& slot : m_textures)
		{
			if(slot.texture) device->DestroyTexture(slot.texture);
		}
		m_textures.clear();
		m_indices.clear();
	}

void Renderer::Impl::UpdateMaterialStates(const Scene& scene)
{
	const uint32_t materialCount = scene.Materials().size();
	constexpr std::array<uint32_t,ToIndex(MaterialTextureKind::Count)> defaults = {0u,0u,1u,0u,2u};
	for(uint32_t materialIndex = 0; materialIndex < materialCount; ++materialIndex)
	{
		const MaterialDesc& material = scene.Materials()[materialIndex];
		SceneMaterialState& materialState = materialStates[materialIndex];
		materialState.textures[ToIndex(MaterialTextureKind::BaseColor)] =
			ResolveTexture(material.baseColorTexture);
		materialState.textures[ToIndex(MaterialTextureKind::MetallicRoughness)] =
			ResolveTexture(material.metallicRoughnessTexture);
		materialState.textures[ToIndex(MaterialTextureKind::Normal)] =
			ResolveTexture(material.normalTexture);
		materialState.textures[ToIndex(MaterialTextureKind::Occlusion)] =
			ResolveTexture(material.occlusionTexture);
		materialState.textures[ToIndex(MaterialTextureKind::Emissive)] =
			ResolveTexture(material.emissiveTexture);

		materialState.textureFlags = 0u;
		for(uint32_t slot = 0; slot < materialState.textures.size(); ++slot)
		{
			auto& texture = materialState.textures[slot];
			if(texture != nullptr)
				materialState.textureFlags |= 1u << slot;
			else
				texture = defaultMaterialTextures[defaults[slot]];
		}
	}
}

	RHI::TextureHandle Renderer::Impl::ResolveTexture(const Image& image) const
	{
		const auto found = m_indices.find({image.GetPixels().data(), image.GetColorSpace()});
		if(found == m_indices.end()) return nullptr;
		const TextureSlot& slot = m_textures[found->second];
		return slot.state == RHI::ResourceState::ShaderResource ? slot.texture : nullptr;
	}
}
