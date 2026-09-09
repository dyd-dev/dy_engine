#include "MetalResourceSet.h"

#include "MetalPipeline.h"
#include "MetalTexture.h"

#import <Metal/Metal.h>

namespace dy::Backends
{
	namespace
	{
		[[nodiscard]] const RHI::ResourceBindingLayout* FindLayoutBinding(
			const RHI::PipelineLayoutDesc& layout,
			uint32_t binding)
		{
			for(uint32_t index = 0; index < layout.bindingCount; ++index)
			{
				const RHI::ResourceBindingLayout& candidate = layout.bindings[index];
				if(candidate.binding == binding)
					return &candidate;
			}
			return nullptr;
		}

		[[nodiscard]] bool ResolveTextureRange(
			MetalTexture* texture,
			const RHI::ResourceBinding& binding,
			RHI::ResourceBindingType type,
			uint32_t& firstMip,
			uint32_t& mipCount,
			uint32_t& firstLayer,
			uint32_t& layerCount)
		{
			firstMip = binding.subresources.firstMipLevel;
			firstLayer = binding.subresources.firstArrayLayer;
			if(firstMip >= texture->GetDesc().mipLevels ||
				firstLayer >= texture->GetDesc().depthOrArraySize)
			{
				return false;
			}
			mipCount = binding.subresources.mipLevelCount == 0
				? texture->GetDesc().mipLevels - firstMip
				: binding.subresources.mipLevelCount;
			layerCount = binding.subresources.arrayLayerCount == 0
				? texture->GetDesc().depthOrArraySize - firstLayer
				: binding.subresources.arrayLayerCount;
			if(mipCount > texture->GetDesc().mipLevels - firstMip ||
				layerCount > texture->GetDesc().depthOrArraySize - firstLayer)
			{
				return false;
			}
			return type != RHI::ResourceBindingType::StorageTexture || mipCount == 1;
		}
	}

	struct MetalResourceSet::Impl
	{
		std::vector<MetalTextureBinding> textures;
		std::vector<void*> ownedTextureViews;
	};

	MetalResourceSet::MetalResourceSet(const RHI::ResourceSetDesc& desc)
		: RHI::ResourceSet(desc)
		, m_impl(new Impl())
	{
		auto* pipeline = dynamic_cast<MetalPipeline*>(GetPipeline());
		if(pipeline == nullptr || pipeline->GetNativePipeline() == nullptr ||
			(pipeline->GetDesc().depthStencil.format != RHI::Format::Unknown &&
				pipeline->GetNativeDepthStencil() == nullptr) ||
			(desc.bindingCount != 0 && desc.bindings == nullptr)) return;
		const RHI::PipelineLayoutDesc& layout = pipeline->GetLayout();

		for(uint32_t index = 0; index < GetBindingCount(); ++index)
		{
			const RHI::ResourceBinding& binding = GetBindings()[index];
			const RHI::ResourceBindingLayout* declaration =
				FindLayoutBinding(layout, binding.binding);
			if(declaration == nullptr ||
				(declaration->type != RHI::ResourceBindingType::SampledTexture &&
					declaration->type != RHI::ResourceBindingType::StorageTexture)) continue;

			auto* texture = dynamic_cast<MetalTexture*>(binding.texture);
			uint32_t firstMip = 0;
			uint32_t mipCount = 0;
			uint32_t firstLayer = 0;
			uint32_t layerCount = 0;
			if(texture == nullptr || texture->GetNativeTexture() == nullptr ||
				!ResolveTextureRange(
					texture, binding, declaration->type,
					firstMip, mipCount, firstLayer, layerCount)) return;

			id<MTLTexture> original =
				(__bridge id<MTLTexture>)texture->GetNativeTexture();
			id<MTLTexture> native = original;
			const bool fullView = firstMip == 0 && mipCount == texture->GetDesc().mipLevels &&
				firstLayer == 0 && layerCount == texture->GetDesc().depthOrArraySize;
			if(!fullView)
			{
				const MTLTextureType viewType = texture->GetDesc().depthOrArraySize > 1
					? MTLTextureType2DArray : MTLTextureType2D;
				native = [original
					newTextureViewWithPixelFormat:original.pixelFormat
					textureType:viewType
					levels:NSMakeRange(firstMip, mipCount)
					slices:NSMakeRange(firstLayer, layerCount)];
				if(native == nil) return;
				void* ownedView = nullptr;
#if __has_feature(objc_arc)
				ownedView = (__bridge_retained void*)native;
#else
				ownedView = (__bridge void*)native;
#endif
				m_impl->ownedTextureViews.push_back(ownedView);
			}
			m_impl->textures.push_back({
				texture,
				binding.binding + binding.arrayElement,
				declaration->stages,
				declaration->type == RHI::ResourceBindingType::SampledTexture
					? RHI::ResourceState::ShaderResource
					: RHI::ResourceState::UnorderedAccess,
				firstMip,
				mipCount,
				firstLayer,
				layerCount,
				(__bridge void*)native});
		}
	}

	MetalResourceSet::~MetalResourceSet()
	{
		if(m_impl == nullptr) return;
#if __has_feature(objc_arc)
		for(void* view : m_impl->ownedTextureViews)
		{
			id<MTLTexture> released = (__bridge_transfer id<MTLTexture>)view;
			(void)released;
		}
#else
		for(void* view : m_impl->ownedTextureViews)
			[(__bridge id<MTLTexture>)view release];
#endif
		m_impl->ownedTextureViews.clear();
		delete m_impl;
	}

	const std::vector<MetalTextureBinding>& MetalResourceSet::GetTextureBindings() const
	{
		return m_impl->textures;
	}
}
