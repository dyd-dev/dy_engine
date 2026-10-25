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
	// 재질 이미지를 RHI 캐시 슬롯에 맞추고 같은 픽셀·색 공간은 공유하며, ShaderResource로의 업로드 제출이 미확정인 슬롯만 처리하고 실패하면 false를 반환한다.
	bool Renderer::Impl::SyncTextures(const Scene& scene, RHI::IDevice* device)
	{
		if(device == nullptr) return false; // 장치가 없으면 캐시 동기화를 시작하지 않는다.

		for(auto& slot:m_textures) slot.used=false;
		std::vector<uint32_t> pending;
		for(const auto& material : scene.Materials())
		{
			for(const Image* image : {&material.baseColorTexture, &material.metallicRoughnessTexture,
				&material.normalTexture, &material.occlusionTexture, &material.emissiveTexture})
			{
				// 유효한 이미지의 픽셀 소유자·색 공간 키를 찾아 기존 슬롯을 재사용한다.
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
		// Image 소유자를 유지해 픽셀 주소 재사용을 구분하고 재질 순서가 바뀌어도 슬롯 정체성을 유지한다. 쓰이지 않는 소유자만 반납한다.
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
		if(pending.empty()) return true; // 추가 업로드가 없으면 현재 캐시로 성공한다.

		// 작업 스레드가 만든 축소 레벨들과 성공 여부만 넘긴다. 원본 mip 0은 Image에서 읽는다.
		struct PreparedMips
		{
			std::vector<std::vector<uint8_t>> levels;
            const Detail::ImageDdsData* dds = nullptr;
            RHI::Format format = RHI::Format::Unknown;
			bool valid = true;
		};
		// 보존 DDS를 쓸 수 없으면 1×1까지 mip을 만든다. sRGB는 선형 광량으로, 데이터 이미지는 성분별로 축소한다.
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
				auto* resized = srgb
					? stbir_resize_uint8_srgb(source, static_cast<int>(width), static_cast<int>(height), 0,
						next.data(), static_cast<int>(nextWidth), static_cast<int>(nextHeight), 0, STBIR_RGBA)
					: stbir_resize_uint8_linear(source, static_cast<int>(width), static_cast<int>(height), 0,
						next.data(), static_cast<int>(nextWidth), static_cast<int>(nextHeight), 0, STBIR_4CHANNEL);
				if(!resized) { result.valid = false; break; } // mip 생성 실패를 기록하고 이 이미지의 준비를 중단한다.
				result.levels.push_back(std::move(next));
				source = result.levels.back().data();
				width = nextWidth;
				height = nextHeight;
			}
			return result;
		};
		// 설정된 동시 준비 수와 남은 이미지 수 중 작은 개수만 시작한다. 한 작업이면 deferred로 두어 아래 get 호출에서 계산하고, 여러 작업이면 비동기로 mip을 준비한다.
		const size_t preparationCount = std::min<size_t>(config.texturePreparationThreadCount, pending.size());
		const auto launch = preparationCount == 1 ? std::launch::deferred : std::launch::async;
		// future를 참조 대상인 이미지 목록보다 먼저 소멸시켜, 실패 반환에서도 실행 중인 CPU 작업이 끝난 뒤 입력을 정리한다.
		// 아직 시작하지 않은 deferred 작업은 실행하지 않는다.
		std::deque<std::future<PreparedMips>> prepared;
		size_t nextPreparation = 0;
		for(; nextPreparation < preparationCount; ++nextPreparation)
			prepared.emplace_back(std::async(launch, prepareMips, pending[nextPreparation],true));

		// 이미지마다 GPU 완료를 확인하는 Wait 한 번의 상한은 5초다. CPU mip 준비나 모든 이미지의 처리 시간을 합친 전체 로딩 시간의 제한은 아니다.
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

			// 원본 해상도와 색 공간으로 GPU 형식 및 전체 mip 개수를 정한다.
			RHI::TextureDesc textureDesc;
			textureDesc.width = image.GetWidth();
			textureDesc.height = image.GetHeight();
			for(uint32_t dimension = std::max(textureDesc.width, textureDesc.height); dimension > 1; dimension >>= 1)
				++textureDesc.mipLevels;
			textureDesc.format = mips.format;
			textureDesc.usage = RHI::TextureUsage::ShaderResource;
            if(mips.dds && mips.dds->mips.size()!=textureDesc.mipLevels)
                return fail("preserved DDS mip count mismatch.");

			if(slot.texture == nullptr) slot.texture = device->CreateTexture(textureDesc); // GPU 텍스처가 없으면 필요한 mip 수로 생성한다.
            if(slot.texture==nullptr && mips.dds)
            {
                // Optional native compression can fail independently of decoding.
                // Keep the original RGBA upload available on unsupported devices.
                mips=prepareMips(textureIndex,false);
                if(!mips.valid) return fail("fallback mipmap generation failed.");
                textureDesc.format=mips.format;
                slot.texture=device->CreateTexture(textureDesc);
            }
			if(slot.texture == nullptr) // 생성 후에도 핸들이 없으면 이 업로드를 실패시킨다.
				return fail("GPU texture allocation failed.");

			RHI::FenceHandle completion;
			{
				// 장면 전체를 중복 기록하지 않도록 한 이미지의 기록 바이트·native staging만 유지한다. 명령 소유권을 놓은 뒤에도 제출 참조는 RHI가 보존한다.
				RHI::ResourceScope resources(*device);
				auto* commandList = device->AcquireCommandList();
				if(commandList == nullptr) return fail("command list allocation failed."); // 명령 리스트를 얻지 못하면 업로드를 실패시킨다.
				resources.Keep(commandList);

				RHI::ResourceBarrierDesc barrier = {
					nullptr, slot.texture, slot.state, RHI::ResourceState::CopyDestination, {}
				};
				commandList->ResourceBarrier(&barrier, 1);
				uint32_t width = textureDesc.width, height = textureDesc.height;
				// 각 레벨의 행 간격과 크기를 계산해 업로드를 기록하고, 마지막에 ShaderResource로 전이한다.
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
					if(!device->UpdateTexture(*commandList, slot.texture, mip, 0, // 한 mip이라도 업로드 기록이 실패하면 제출하지 않는다.
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
				if(!commandList->Close() || !device->Submit({&commandList, 1, nullptr, 0}, completion)) // 명령 닫기 또는 제출이 실패하면 slot.state를 ShaderResource로 확정하지 않고 false를 반환한다.
					return fail("GPU submission failed.");

				// 제출된 명령의 예정 상태는 다음 Wait가 실패해도 되돌리지 않으며, 실행 중인 자료는 네이티브 명령이 계속 보존한다.
				slot.state = barrier.after;
			}
			// Release the recorded CPU copy before waiting. Retiring this submission
			// also releases its native staging pages before the next image is copied.
			if(!device->Wait(completion, uploadTimeoutNanoseconds)) // 제출 완료를 5초 안에 확인하지 못하면 장치 손실과 시간 초과를 구분해 알린다.
				return fail(device->IsLost() ? "device became unavailable while waiting for the upload."
					: "GPU upload did not complete within five seconds.");
            if(mips.dds) ++compressedTextures;
			if(nextPreparation < pending.size()) // 한 이미지 업로드가 끝나면 다음 CPU 준비 작업을 보충한다.
				prepared.emplace_back(std::async(launch, prepareMips, pending[nextPreparation++],true));
		}
        std::fprintf(stderr,"[Renderer] Material texture uploads: %zu images, %u preserved DDS BC1/BC3; payload=%llu bytes, RGBA equivalent=%llu bytes.\n",
            pending.size(),compressedTextures,static_cast<unsigned long long>(uploadedBytes),static_cast<unsigned long long>(rgbaBytes));
		return true;
	}

	// 소유한 GPU 텍스처를 RHI에 반납하고 Image 참조·인덱스 맵을 비운다.
	void Renderer::Impl::ReleaseTextures(RHI::IDevice* device)
	{
		for(TextureSlot& slot : m_textures)
		{
			if(slot.texture) device->DestroyTexture(slot.texture); // 종료하는 캐시 슬롯의 GPU 텍스처를 반납한다.
		}
		m_textures.clear();
		m_freeTextureSlots.clear();
		m_indices.clear();
	}

// 재질의 다섯 이미지 슬롯을 GPU 핸들로 바꾸고 실제 텍스처가 있는 슬롯만 textureFlags에 표시한다.
void Renderer::Impl::UpdateMaterialStates(const Scene& scene)
{
	const uint32_t materialCount = scene.Materials().size();
	const bool stockShaders = UsesStockGeometry();
	// 이미지가 없는 슬롯에는 흰색, 표면에 수직인 방향(법선)을 담는 기본 텍스처, 검정 등 용도별 대체 텍스처를 넣는다.
	// 대체 핸들이 있어도 textureFlags의 해당 비트는 켜지지 않으므로 셰이더는 원래 재질 이미지가 있었는지 구분할 수 있다.
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
			if(texture != nullptr) // 실제 이미지 핸들이 있을 때만 해당 텍스처 비트를 켠다.
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

	// 픽셀 주소와 색 공간으로 찾되 제출 상태가 ShaderResource인 슬롯만 반환한다. 이 조회 자체가 GPU 완료를 기다리지는 않는다.
	RHI::TextureHandle Renderer::Impl::ResolveTexture(const Image& image) const
	{
		const auto found = m_indices.find({image.GetPixels().data(), image.GetColorSpace()});
		if(found == m_indices.end()) return nullptr; // 현재 재질 이미지 목록에 없는 입력이면 nullptr다.
		const TextureSlot& slot = m_textures[found->second];
		return slot.state == RHI::ResourceState::ShaderResource ? slot.texture : nullptr;
	}
}
