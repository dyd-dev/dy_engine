#include "dyf/Image.h"
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

		m_indices.clear();
		std::vector<const Image*> images;
		for(const auto& material : scene.Materials())
		{
			for(const Image* image : {&material.baseColorTexture, &material.metallicRoughnessTexture,
				&material.normalTexture, &material.occlusionTexture, &material.emissiveTexture})
			{
				if(image->IsValid() && m_indices.emplace( // 유효하고 아직 같은 키가 없을 때만 이미지 목록에 추가한다.
					std::make_pair(image->GetPixels().data(), image->GetColorSpace()),
					static_cast<uint32_t>(images.size())).second)
				{
					images.push_back(image);
				}
			}
		}

		// 현재 재질 목록보다 많이 남은 슬롯을 줄인다. 각 슬롯이 가진 Image는 픽셀 주소 재사용에 따른 오인식을 막는다.
		while(m_textures.size() > images.size())
		{
			if(m_textures.back().texture) device->DestroyTexture(m_textures.back().texture); // 잘라낼 슬롯에 GPU 텍스처가 있으면 반납한다.
			m_textures.pop_back();
		}
		m_textures.resize(images.size());

		// 픽셀 소유자나 색 공간이 바뀌면 이전 GPU 텍스처를 놓고 재업로드 대상을 만든다.
		std::vector<uint32_t> pending;
		for(uint32_t textureIndex = 0; textureIndex < images.size(); ++textureIndex)
		{
			const Image& image = *images[textureIndex];
			TextureSlot& slot = m_textures[textureIndex];
			if(slot.source.GetPixels().data() != image.GetPixels().data() ||
				slot.source.GetColorSpace() != image.GetColorSpace())
			{
				if(slot.texture) device->DestroyTexture(slot.texture); // 이미지가 바뀐 슬롯의 이전 GPU 텍스처를 반납한다.
				slot = {};
				slot.source = image;
			}
			if(slot.state == RHI::ResourceState::ShaderResource) continue; // 이미 제출된 슬롯은 다시 업로드하지 않는다.
			if(image.GetPixels().size() > std::numeric_limits<uint32_t>::max() || // 업로드 크기와 축소 라이브러리 정수 범위를 넘어가면 실패한다.
				image.GetWidth() > static_cast<uint32_t>(std::numeric_limits<int>::max() / 4) ||
				image.GetHeight() > static_cast<uint32_t>(std::numeric_limits<int>::max()))
			{
				std::fprintf(stderr, "dyf: texture %u/%zu exceeds the upload or mipmap size limit.\n",
					textureIndex + 1, images.size());
				return false;
			}
			pending.push_back(textureIndex);
		}
		if(pending.empty()) return true; // 추가 업로드가 없으면 현재 캐시로 성공한다.

		// 작업 스레드가 만든 축소 레벨들과 성공 여부만 넘긴다. 원본 mip 0은 Image에서 읽는다.
		struct PreparedMips
		{
			std::vector<std::vector<uint8_t>> levels;
			bool valid = true;
		};
		// 각 축을 절반씩 줄여 1×1까지 만든다. sRGB 색은 선형 광량으로 필터링하고 데이터 이미지는 성분별로 축소한다.
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
			prepared.emplace_back(std::async(launch, prepareMips, pending[nextPreparation]));

		// 이미지마다 GPU 완료를 확인하는 Wait 한 번의 상한은 5초다. CPU mip 준비나 모든 이미지의 처리 시간을 합친 전체 로딩 시간의 제한은 아니다.
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

			// 원본 해상도와 색 공간으로 GPU 형식 및 전체 mip 개수를 정한다.
			RHI::TextureDesc textureDesc;
			textureDesc.width = image.GetWidth();
			textureDesc.height = image.GetHeight();
			for(uint32_t dimension = std::max(textureDesc.width, textureDesc.height); dimension > 1; dimension >>= 1)
				++textureDesc.mipLevels;
			const bool srgb = image.GetColorSpace() == ColorSpace::Srgb;
			textureDesc.format = srgb
				? RHI::Format::R8G8B8A8_UNORM_SRGB : RHI::Format::R8G8B8A8_UNORM;
			textureDesc.usage = RHI::TextureUsage::ShaderResource;

			if(slot.texture == nullptr) slot.texture = device->CreateTexture(textureDesc); // GPU 텍스처가 없으면 필요한 mip 수로 생성한다.
			if(slot.texture == nullptr) // 생성 후에도 핸들이 없으면 이 업로드를 실패시킨다.
				return fail("GPU texture allocation failed.");

			RHI::FenceHandle completion;
			{
				PreparedMips mips = prepared.front().get();
				prepared.pop_front();
				if(!mips.valid) return fail("mipmap generation failed."); // CPU mip 생성이 실패했으면 GPU 기록을 시작하지 않는다.
				// 장면 전체를 중복 기록하지 않도록 한 이미지의 기록 바이트·native staging만 유지하며, 명령 소유권을 놓은 뒤에도 GPU에 필요한 제출 참조는 RHI가 보존한다.
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
					const uint8_t* mipData = mip == 0 ? image.GetPixels().data() : mips.levels[mip - 1].data();
					const uint32_t rowPitch = width * 4;
					const uint32_t uploadSize = rowPitch * height;
					if(!device->UpdateTexture(*commandList, slot.texture, mip, 0, // 한 mip이라도 업로드 기록이 실패하면 제출하지 않는다.
						mipData, uploadSize, rowPitch, uploadSize))
						return fail("pixel upload recording failed.");
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
			if(nextPreparation < pending.size()) // 한 이미지 업로드가 끝나면 다음 CPU 준비 작업을 보충한다.
				prepared.emplace_back(std::async(launch, prepareMips, pending[nextPreparation++]));
		}
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
		m_indices.clear();
	}

// 재질의 다섯 이미지 슬롯을 GPU 핸들로 바꾸고 실제 텍스처가 있는 슬롯만 textureFlags에 표시한다.
void Renderer::Impl::UpdateMaterialStates(const Scene& scene)
{
	const uint32_t materialCount = scene.Materials().size();
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
