#include "dyf/Image.h"
#include "ImageDds.h"
#include "RHI/FormatCapabilities.h"
#include "RendererInternal.h"
#include "dyf/Scene.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/Texture.h"

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

		for(auto& slot:m_textures) slot.used=false;
		std::vector<uint32_t> pending;
		for(const auto& material : scene.Materials())
		{
			for(const Image* image : {&material.baseColorTexture, &material.metallicRoughnessTexture,
				&material.normalTexture, &material.occlusionTexture, &material.emissiveTexture})
			{
				if(!image->IsValid()) continue;
				const auto key=std::make_pair(image->GetPixels().data(),image->GetColorSpace());
				auto found=m_indices.find(key);
				if(found==m_indices.end())
				{
					Image source=*image;
					const bool append=m_freeTextureSlots.empty();
					if(append && m_textures.size()>=UINT32_MAX) return false;
					const auto index=append ? static_cast<uint32_t>(m_textures.size()) : m_freeTextureSlots.back();
					if(append) m_textures.emplace_back();
					try {found=m_indices.emplace(key,index).first;}
					catch(...) {if(append)m_textures.pop_back();throw;}
					if(!append) m_freeTextureSlots.pop_back();
					m_textures[index].source=std::move(source);
				}
				auto& slot=m_textures[found->second];
				if(slot.used) continue;
				slot.used=true;
				if(slot.state==RHI::ResourceState::ShaderResource) continue;
				if(image->GetPixels().size()>std::numeric_limits<uint32_t>::max() ||
					image->GetWidth()>static_cast<uint32_t>(std::numeric_limits<int>::max()/4) ||
					image->GetHeight()>static_cast<uint32_t>(std::numeric_limits<int>::max()))
				{
					std::fprintf(stderr,"dyf: texture exceeds the upload or mipmap size limit.\n");
					return false;
				}
				pending.push_back(found->second);
			}
		}
		// Stable identities survive material reordering. Only unused owners are retired.
		m_freeTextureSlots.reserve(m_textures.size());
		for(uint32_t i=0;i<m_textures.size();++i)
		{
			auto& slot=m_textures[i];
			if(!slot.used && slot.source.IsValid())
			{
				device->DestroyTexture(slot.texture);
				m_indices.erase({slot.source.GetPixels().data(),slot.source.GetColorSpace()});
				slot={};
				m_freeTextureSlots.push_back(i);
			}
		}
		if(pending.empty()) return true;

		struct PreparedMips
		{
			std::vector<std::vector<uint8_t>> levels;
            const Detail::ImageDdsData* dds = nullptr;
            RHI::Format format = RHI::Format::Unknown;
			bool valid = true;
		};
		const auto prepareMips = [&](uint32_t textureIndex,bool allowCompressed) {
			const Image& image = m_textures[textureIndex].source;
			PreparedMips result;
			uint32_t width = image.GetWidth(), height = image.GetHeight();
			const uint8_t* source = image.GetPixels().data();
			const bool srgb = image.GetColorSpace() == ColorSpace::Srgb;
            result.format=srgb ? RHI::Format::R8G8B8A8_UNORM_SRGB : RHI::Format::R8G8B8A8_UNORM;
            if(allowCompressed)
                if(const auto* dds=Detail::ImageDdsAccess::Get(image))
                {
                    auto format=dds->format;
                    if(srgb) format=format==RHI::Format::BC1_UNORM ? RHI::Format::BC1_UNORM_SRGB : RHI::Format::BC3_UNORM_SRGB;
                    if(RHI::Detail::SupportsDdsCompression(*device,format))
                    {
                        result.dds=dds;result.format=format;
                        return result;
                    }
                }
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
			prepared.emplace_back(std::async(launch, prepareMips, pending[nextPreparation],true));

		constexpr uint64_t uploadTimeoutNanoseconds = 5000000000ull;
        uint64_t uploadedBytes=0,rgbaBytes=0;
        uint32_t compressedTextures=0;
		for(uint32_t textureIndex : pending)
		{
			const Image& image = m_textures[textureIndex].source;
			const auto fail = [&](const char* reason) {
				std::fprintf(stderr, "dyf: texture upload %u/%zu (%u x %u): %s\n",
					textureIndex + 1, m_textures.size(), image.GetWidth(), image.GetHeight(), reason);
				return false;
			};
			TextureSlot& slot = m_textures[textureIndex];
            PreparedMips mips=prepared.front().get();
            prepared.pop_front();
            if(!mips.valid) return fail("mipmap generation failed.");

			RHI::TextureDesc textureDesc;
			textureDesc.width = image.GetWidth();
			textureDesc.height = image.GetHeight();
			for(uint32_t dimension = std::max(textureDesc.width, textureDesc.height); dimension > 1; dimension >>= 1)
				++textureDesc.mipLevels;
			textureDesc.format = mips.format;
			textureDesc.usage = RHI::TextureUsage::ShaderResource;
            if(mips.dds && mips.dds->mips.size()!=textureDesc.mipLevels)
                return fail("preserved DDS mip count mismatch.");

			if(slot.texture == nullptr) slot.texture = device->CreateTexture(textureDesc);
            if(slot.texture==nullptr && mips.dds)
            {
                // Optional native compression can fail independently of decoding.
                // Keep the original RGBA upload available on unsupported devices.
                mips=prepareMips(textureIndex,false);
                if(!mips.valid) return fail("fallback mipmap generation failed.");
                textureDesc.format=mips.format;
                slot.texture=device->CreateTexture(textureDesc);
            }
			if(slot.texture == nullptr)
				return fail("GPU texture allocation failed.");

			RHI::FenceHandle completion;
			{
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
                    const bool compressed=mips.dds!=nullptr;
                    const uint8_t* mipData=compressed ? mips.dds->mips[mip].data() :
                        mip==0 ? image.GetPixels().data() : mips.levels[mip-1].data();
                    const uint32_t rowPitch=compressed ? ((width+3)/4)*RHI::CompressedBlockBytes(textureDesc.format) : width*4;
                    const uint32_t rows=compressed ? (height+3)/4 : height;
					const uint32_t uploadSize = rowPitch * rows;
                    if(compressed && mips.dds->mips[mip].size()!=uploadSize)
                        return fail("preserved DDS mip byte count mismatch.");
					if(!device->UpdateTexture(*commandList, slot.texture, mip, 0,
						mipData, uploadSize, rowPitch, uploadSize))
						return fail("pixel upload recording failed.");
                    uploadedBytes+=uploadSize;
                    rgbaBytes+=uint64_t(width)*height*4;
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
            if(mips.dds) ++compressedTextures;
			if(nextPreparation < pending.size())
				prepared.emplace_back(std::async(launch, prepareMips, pending[nextPreparation++],true));
		}
        std::fprintf(stderr,"[Renderer] Material texture uploads: %zu images, %u preserved DDS BC1/BC3; payload=%llu bytes, RGBA equivalent=%llu bytes.\n",
            pending.size(),compressedTextures,static_cast<unsigned long long>(uploadedBytes),static_cast<unsigned long long>(rgbaBytes));
		return true;
	}

	void Renderer::Impl::ReleaseTextures(RHI::IDevice* device)
	{
		for(TextureSlot& slot : m_textures)
		{
			if(slot.texture) device->DestroyTexture(slot.texture);
		}
		m_textures.clear();
		m_freeTextureSlots.clear();
		m_indices.clear();
	}

void Renderer::Impl::UpdateMaterialStates(const Scene& scene)
{
	const uint32_t materialCount = scene.Materials().size();
	const bool stockShaders = UsesStockGeometry();
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
		// These samples cannot contribute to the stock shading result. Keep the
		// texture owners/bindings, and preserve presence flags for custom shaders.
		if(stockShaders)
		{
			if(material.occlusionStrength == 0.0f)
				materialState.textureFlags &= ~(1u << ToIndex(MaterialTextureKind::Occlusion));
			if(material.emissiveColor.x == 0.0f && material.emissiveColor.y == 0.0f && material.emissiveColor.z == 0.0f)
				materialState.textureFlags &= ~(1u << ToIndex(MaterialTextureKind::Emissive));
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
