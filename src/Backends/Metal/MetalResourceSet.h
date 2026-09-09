#pragma once

#include <cstdint>
#include <vector>

#include "RHI/ResourceSet.h"
#include "RHI/ResourceState.h"

namespace dy::Backends
{
	class MetalTexture;
	struct MetalObjectDeleter;

	struct MetalTextureBinding
	{
		MetalTexture* texture = nullptr;
		uint32_t index = 0;
		RHI::ShaderStageFlags stages = RHI::ShaderStageFlags::None;
		RHI::ResourceState requiredState = RHI::ResourceState::Undefined;
		uint32_t firstMipLevel = 0;
		uint32_t mipLevelCount = 0;
		uint32_t firstArrayLayer = 0;
		uint32_t arrayLayerCount = 0;
		void* nativeTexture = nullptr;
	};

	class MetalResourceSet final : public RHI::ResourceSet
	{
	public:
		explicit MetalResourceSet(const RHI::ResourceSetDesc& desc);

		[[nodiscard]] const std::vector<MetalTextureBinding>& GetTextureBindings() const;

	private:
		friend struct MetalObjectDeleter;

		~MetalResourceSet() override;

		struct Impl;
		Impl* m_impl = nullptr;
	};
}
