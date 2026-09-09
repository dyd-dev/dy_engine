#include "NullDevice.h"

#include "RHI/Buffer.h"
#include "RHI/ICommandList.h"
#include "RHI/Pipeline.h"
#include "RHI/ResourceSet.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dy::Backends
{
	namespace
	{
		[[nodiscard]] bool HasUsage(RHI::BufferUsage value, RHI::BufferUsage usage)
		{
			return (value & usage) != RHI::BufferUsage::None;
		}

		[[nodiscard]] bool HasUsage(RHI::TextureUsage value, RHI::TextureUsage usage)
		{
			return (value & usage) != RHI::TextureUsage::None;
		}

		[[nodiscard]] uint32_t FormatSize(RHI::Format format)
		{
			switch(format)
			{
			case RHI::Format::R8G8B8A8_UNORM:
			case RHI::Format::B8G8R8A8_UNORM:
			case RHI::Format::R8G8B8A8_UNORM_SRGB:
			case RHI::Format::B8G8R8A8_UNORM_SRGB:
			case RHI::Format::D32_FLOAT:
			case RHI::Format::D24_UNORM_S8_UINT:
			case RHI::Format::R32_UINT:
				return 4;
			case RHI::Format::R16G16B16A16_FLOAT:
			case RHI::Format::R32G32_FLOAT:
				return 8;
			case RHI::Format::R32G32B32_FLOAT:
				return 12;
			case RHI::Format::R32G32B32A32_FLOAT:
				return 16;
			case RHI::Format::R16_UINT:
				return 2;
			default:
				return 0;
			}
		}

		[[nodiscard]] bool IsDepthFormat(RHI::Format format)
		{
			return format == RHI::Format::D32_FLOAT ||
				format == RHI::Format::D24_UNORM_S8_UINT;
		}

		[[nodiscard]] bool IsColorFormat(RHI::Format format)
		{
			return FormatSize(format) != 0 && !IsDepthFormat(format);
		}

		[[nodiscard]] uint32_t MaximumMipCount(uint32_t width, uint32_t height)
		{
			uint32_t dimension = std::max(width, height);
			uint32_t result = 0;
			while(dimension != 0)
			{
				++result;
				dimension >>= 1u;
			}
			return result;
		}

		[[nodiscard]] bool IsBufferStateAllowed(
			const RHI::BufferDesc& desc,
			RHI::ResourceState state)
		{
			switch(state)
			{
			case RHI::ResourceState::Undefined:
			case RHI::ResourceState::Common:
			case RHI::ResourceState::CopyDestination:
				return true;
			case RHI::ResourceState::VertexBuffer:
				return HasUsage(desc.usage, RHI::BufferUsage::Vertex);
			case RHI::ResourceState::IndexBuffer:
				return HasUsage(desc.usage, RHI::BufferUsage::Index);
			case RHI::ResourceState::ConstantBuffer:
				return HasUsage(desc.usage, RHI::BufferUsage::Constant);
			case RHI::ResourceState::ShaderResource:
			case RHI::ResourceState::UnorderedAccess:
				return HasUsage(desc.usage, RHI::BufferUsage::Storage);
			default:
				return false;
			}
		}

		[[nodiscard]] bool IsTextureStateAllowed(
			const RHI::TextureDesc& desc,
			RHI::ResourceState state)
		{
			switch(state)
			{
			case RHI::ResourceState::Undefined:
			case RHI::ResourceState::Common:
			case RHI::ResourceState::CopyDestination:
				return true;
			case RHI::ResourceState::ShaderResource:
				return HasUsage(desc.usage, RHI::TextureUsage::ShaderResource);
			case RHI::ResourceState::UnorderedAccess:
				return HasUsage(desc.usage, RHI::TextureUsage::Storage);
			case RHI::ResourceState::RenderTarget:
				return HasUsage(desc.usage, RHI::TextureUsage::RenderTarget);
			case RHI::ResourceState::DepthRead:
			case RHI::ResourceState::DepthWrite:
				return HasUsage(desc.usage, RHI::TextureUsage::DepthStencil);
			case RHI::ResourceState::Present:
				return HasUsage(desc.usage, RHI::TextureUsage::RenderTarget);
			default:
				return false;
			}
		}

		class NullBuffer final : public RHI::Buffer
		{
		public:
			explicit NullBuffer(const RHI::BufferDesc& desc)
				: RHI::Buffer(desc)
				, m_data(desc.size)
				, m_state(desc.initialState)
			{
			}

			[[nodiscard]] RHI::ResourceState GetState() const { return m_state; }
			void SetState(RHI::ResourceState state) { m_state = state; }

			void Write(uint32_t offset, const std::vector<uint8_t>& data)
			{
				std::memcpy(m_data.data() + offset, data.data(), data.size());
			}

		private:
			std::vector<uint8_t> m_data;
			RHI::ResourceState m_state = RHI::ResourceState::Undefined;
		};

		class NullTexture final : public RHI::Texture
		{
		public:
			explicit NullTexture(
				const RHI::TextureDesc& desc,
				bool swapchainImage = false)
				: RHI::Texture(desc)
				, m_states(
					static_cast<size_t>(desc.mipLevels) * desc.depthOrArraySize,
					swapchainImage
						? RHI::ResourceState::Present
						: RHI::ResourceState::Undefined)
				, m_swapchainImage(swapchainImage)
			{
			}

			[[nodiscard]] bool IsSwapchainImage() const { return m_swapchainImage; }
			[[nodiscard]] RHI::ResourceState GetState(
				uint32_t mipLevel,
				uint32_t arrayLayer) const
			{
				if(mipLevel >= GetDesc().mipLevels || arrayLayer >= GetDesc().depthOrArraySize)
					return RHI::ResourceState::Undefined;
				return m_states[static_cast<size_t>(arrayLayer) * GetDesc().mipLevels + mipLevel];
			}
			void SetState(
				uint32_t mipLevel,
				uint32_t arrayLayer,
				RHI::ResourceState state)
			{
				if(mipLevel >= GetDesc().mipLevels || arrayLayer >= GetDesc().depthOrArraySize) return;
				m_states[static_cast<size_t>(arrayLayer) * GetDesc().mipLevels + mipLevel] = state;
			}

			void Write(
				uint32_t mipLevel,
				uint32_t arrayLayer,
				uint32_t rowPitch,
				uint32_t slicePitch,
				const std::vector<uint8_t>& data)
			{
				const size_t index = static_cast<size_t>(arrayLayer) * GetDesc().mipLevels + mipLevel;
				if(m_subresources.size() <
					static_cast<size_t>(GetDesc().depthOrArraySize) * GetDesc().mipLevels)
				{
					m_subresources.resize(
						static_cast<size_t>(GetDesc().depthOrArraySize) * GetDesc().mipLevels);
				}
				m_subresources[index] = {rowPitch, slicePitch, data};
			}

		private:
			struct SubresourceData
			{
				uint32_t rowPitch = 0;
				uint32_t slicePitch = 0;
				std::vector<uint8_t> data;
			};

			std::vector<SubresourceData> m_subresources;
			std::vector<RHI::ResourceState> m_states;
			bool m_swapchainImage = false;
		};

		class NullShader final : public RHI::Shader
		{
		public:
			explicit NullShader(const RHI::ShaderDesc& desc)
				: RHI::Shader(desc)
			{
			}
		};

		[[nodiscard]] std::pair<NullTexture*, uint32_t> TextureKey(
			NullTexture* texture,
			uint32_t mipLevel,
			uint32_t arrayLayer)
		{
			return {texture, arrayLayer * texture->GetDesc().mipLevels + mipLevel};
		}

		[[nodiscard]] bool ResolveSubresources(
			NullTexture* texture,
			const RHI::TextureSubresourceRange& range,
			uint32_t& firstMip,
			uint32_t& mipCount,
			uint32_t& firstLayer,
			uint32_t& layerCount)
		{
			firstMip = range.firstMipLevel;
			firstLayer = range.firstArrayLayer;
			if(firstMip >= texture->GetDesc().mipLevels ||
				firstLayer >= texture->GetDesc().depthOrArraySize)
			{
				return false;
			}
			mipCount = range.mipLevelCount == 0
				? texture->GetDesc().mipLevels - firstMip : range.mipLevelCount;
			layerCount = range.arrayLayerCount == 0
				? texture->GetDesc().depthOrArraySize - firstLayer : range.arrayLayerCount;
			return mipCount <= texture->GetDesc().mipLevels - firstMip &&
				layerCount <= texture->GetDesc().depthOrArraySize - firstLayer;
		}

		[[nodiscard]] bool ResolveBindingSubresources(
			NullTexture* texture,
			const RHI::ResourceBinding& binding,
			RHI::ResourceBindingType type,
			uint32_t& firstMip,
			uint32_t& mipCount,
			uint32_t& firstLayer,
			uint32_t& layerCount)
		{
			if(!ResolveSubresources(
				texture, binding.subresources,
				firstMip, mipCount, firstLayer, layerCount))
			{
				return false;
			}
			return type != RHI::ResourceBindingType::StorageTexture || mipCount == 1;
		}

		[[nodiscard]] bool IsValidStageFlags(RHI::ShaderStageFlags stages)
		{
			constexpr auto all = RHI::ShaderStageFlags::Vertex |
				RHI::ShaderStageFlags::Fragment;
			return stages != RHI::ShaderStageFlags::None &&
				(stages & all) == stages;
		}

		[[nodiscard]] bool IsValidSampler(const RHI::SamplerDesc& desc)
		{
			const bool usesBorder =
				desc.addressU == RHI::SamplerAddressMode::ClampToBorder ||
				desc.addressV == RHI::SamplerAddressMode::ClampToBorder ||
				desc.addressW == RHI::SamplerAddressMode::ClampToBorder;
			return desc.minFilter != RHI::SamplerFilter::Undefined &&
				desc.magFilter != RHI::SamplerFilter::Undefined &&
				desc.mipFilter != RHI::SamplerFilter::Undefined &&
				desc.addressU != RHI::SamplerAddressMode::Undefined &&
				desc.addressV != RHI::SamplerAddressMode::Undefined &&
				desc.addressW != RHI::SamplerAddressMode::Undefined &&
				(!usesBorder || desc.borderColor != RHI::SamplerBorderColor::Undefined) &&
				desc.maxAnisotropy != 0 && std::isfinite(desc.mipLodBias) &&
				std::isfinite(desc.minLod) && std::isfinite(desc.maxLod) &&
				desc.minLod <= desc.maxLod;
		}

		[[nodiscard]] bool ValidatePipelineLayout(const RHI::PipelineLayoutDesc& desc)
		{
			constexpr auto graphicsStages = RHI::ShaderStageFlags::Vertex |
				RHI::ShaderStageFlags::Fragment;
			if((desc.bindingCount != 0 && desc.bindings == nullptr) ||
				(desc.inlineConstantSize != 0 &&
					(!IsValidStageFlags(desc.inlineConstantStages) ||
						(desc.inlineConstantSize % sizeof(uint32_t)) != 0 ||
						(desc.inlineConstantStages & graphicsStages) !=
							desc.inlineConstantStages)))
			{
				return false;
			}

			std::set<uint32_t> occupied;
			for(uint32_t index = 0; index < desc.bindingCount; ++index)
			{
				const RHI::ResourceBindingLayout& binding = desc.bindings[index];
				if(binding.type == RHI::ResourceBindingType::Undefined ||
					binding.count == 0 || !IsValidStageFlags(binding.stages) ||
					(binding.stages & graphicsStages) != binding.stages) return false;
				if(!occupied.insert(binding.binding).second) return false;
				if(binding.type == RHI::ResourceBindingType::StaticSampler &&
					!IsValidSampler(binding.staticSampler))
				{
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] bool ValidateGraphicsPipelineDesc(
			const RHI::GraphicsPipelineDesc& desc)
		{
			if((desc.vertexBufferCount != 0 && desc.vertexBuffers == nullptr) ||
				(desc.vertexAttributeCount != 0 && desc.vertexAttributes == nullptr) ||
				(desc.colorAttachmentCount != 0 && desc.colorAttachments == nullptr) ||
				!ValidatePipelineLayout(desc.layout))
			{
				return false;
			}

			auto* vertexShader = dynamic_cast<NullShader*>(desc.vertexShader);
			if(vertexShader == nullptr || vertexShader->GetStage() != RHI::ShaderStage::Vertex ||
				desc.topology == RHI::PrimitiveTopology::Undefined ||
				desc.raster.fillMode == RHI::FillMode::Undefined ||
				desc.raster.cullMode == RHI::CullMode::Undefined ||
				desc.raster.frontFace == RHI::FrontFace::Undefined ||
				!std::isfinite(desc.raster.depthBiasConstant) ||
				!std::isfinite(desc.raster.depthBiasSlope) ||
				!std::isfinite(desc.raster.depthBiasClamp))
			{
				return false;
			}

			if(desc.fragmentShader != nullptr)
			{
				auto* fragmentShader = dynamic_cast<NullShader*>(desc.fragmentShader);
				if(fragmentShader == nullptr ||
					fragmentShader->GetStage() != RHI::ShaderStage::Fragment)
				{
					return false;
				}
			}
			if(desc.colorAttachmentCount != 0 && desc.fragmentShader == nullptr)
				return false;

			std::set<uint32_t> vertexBindings;
			for(uint32_t index = 0; index < desc.vertexBufferCount; ++index)
			{
				const RHI::VertexBufferLayout& layout = desc.vertexBuffers[index];
				if(layout.stride == 0 || layout.stepMode == RHI::VertexStepMode::Undefined ||
					!vertexBindings.insert(layout.binding).second)
				{
					return false;
				}
			}

			std::set<uint32_t> locations;
			for(uint32_t index = 0; index < desc.vertexAttributeCount; ++index)
			{
				const RHI::VertexAttribute& attribute = desc.vertexAttributes[index];
				const RHI::VertexBufferLayout* layout = nullptr;
				for(uint32_t layoutIndex = 0; layoutIndex < desc.vertexBufferCount; ++layoutIndex)
				{
					if(desc.vertexBuffers[layoutIndex].binding == attribute.binding)
					{
						layout = &desc.vertexBuffers[layoutIndex];
						break;
					}
				}
				const uint32_t size = FormatSize(attribute.format);
				if(layout == nullptr || size == 0 || IsDepthFormat(attribute.format) ||
					attribute.offset > layout->stride || size > layout->stride - attribute.offset ||
					!locations.insert(attribute.location).second)
				{
					return false;
				}
			}

			if(desc.depthStencil.format != RHI::Format::Unknown)
			{
				if(!IsDepthFormat(desc.depthStencil.format)) return false;
				if(desc.depthStencil.depthTestEnabled &&
					desc.depthStencil.depthCompareOp == RHI::CompareOp::Undefined)
				{
					return false;
				}
				if(desc.depthStencil.stencilEnabled)
				{
					const auto validFace = [](const RHI::StencilFaceState& face)
					{
						return face.failOp != RHI::StencilOp::Undefined &&
							face.depthFailOp != RHI::StencilOp::Undefined &&
							face.passOp != RHI::StencilOp::Undefined &&
							face.compareOp != RHI::CompareOp::Undefined;
					};
					if(desc.depthStencil.format != RHI::Format::D24_UNORM_S8_UINT ||
						!validFace(desc.depthStencil.front) ||
						!validFace(desc.depthStencil.back))
					{
						return false;
					}
				}
			}
			else if(desc.depthStencil.depthTestEnabled ||
				desc.depthStencil.depthWriteEnabled || desc.depthStencil.stencilEnabled)
			{
				return false;
			}

			for(uint32_t index = 0; index < desc.colorAttachmentCount; ++index)
			{
				const RHI::ColorAttachmentDesc& attachment = desc.colorAttachments[index];
				if(!IsColorFormat(attachment.format)) return false;
				if(attachment.blend.enabled &&
					(attachment.blend.sourceColor == RHI::BlendFactor::Undefined ||
						attachment.blend.destinationColor == RHI::BlendFactor::Undefined ||
						attachment.blend.colorOp == RHI::BlendOp::Undefined ||
						attachment.blend.sourceAlpha == RHI::BlendFactor::Undefined ||
						attachment.blend.destinationAlpha == RHI::BlendFactor::Undefined ||
						attachment.blend.alphaOp == RHI::BlendOp::Undefined))
				{
					return false;
				}
			}
			return true;
		}

		class NullPipelineState final : public RHI::Pipeline
		{
		public:
			explicit NullPipelineState(const RHI::GraphicsPipelineDesc& desc)
				: RHI::Pipeline(desc.layout)
				, m_desc(desc)
			{
				if(desc.vertexBufferCount != 0)
					m_vertexBuffers.assign(
						desc.vertexBuffers, desc.vertexBuffers + desc.vertexBufferCount);
				if(desc.vertexAttributeCount != 0)
					m_vertexAttributes.assign(
						desc.vertexAttributes, desc.vertexAttributes + desc.vertexAttributeCount);
				if(desc.colorAttachmentCount != 0)
					m_colorAttachments.assign(
						desc.colorAttachments, desc.colorAttachments + desc.colorAttachmentCount);

				m_desc.vertexBuffers = m_vertexBuffers.empty() ? nullptr : m_vertexBuffers.data();
				m_desc.vertexAttributes = m_vertexAttributes.empty() ? nullptr : m_vertexAttributes.data();
				m_desc.colorAttachments = m_colorAttachments.empty() ? nullptr : m_colorAttachments.data();
				m_desc.layout = GetLayout();
			}

			[[nodiscard]] const RHI::GraphicsPipelineDesc& GetDesc() const { return m_desc; }

		private:
			RHI::GraphicsPipelineDesc m_desc = {};
			std::vector<RHI::VertexBufferLayout> m_vertexBuffers;
			std::vector<RHI::VertexAttribute> m_vertexAttributes;
			std::vector<RHI::ColorAttachmentDesc> m_colorAttachments;
		};

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

		[[nodiscard]] bool ValidateResourceSetDesc(const RHI::ResourceSetDesc& desc)
		{
			auto* pipeline = dynamic_cast<NullPipelineState*>(desc.pipeline);
			if(pipeline == nullptr || (desc.bindingCount != 0 && desc.bindings == nullptr))
				return false;
			const RHI::PipelineLayoutDesc& layout = pipeline->GetLayout();

			std::set<std::pair<uint32_t, uint32_t>> populated;
			for(uint32_t index = 0; index < desc.bindingCount; ++index)
			{
				const RHI::ResourceBinding& binding = desc.bindings[index];
				const RHI::ResourceBindingLayout* declaration =
					FindLayoutBinding(layout, binding.binding);
				if(declaration == nullptr ||
					declaration->type == RHI::ResourceBindingType::StaticSampler ||
					binding.arrayElement >= declaration->count ||
					!populated.emplace(binding.binding, binding.arrayElement).second)
				{
					return false;
				}

				switch(declaration->type)
				{
				case RHI::ResourceBindingType::ConstantBuffer:
				case RHI::ResourceBindingType::ReadOnlyStorageBuffer:
				case RHI::ResourceBindingType::ReadWriteStorageBuffer:
				{
					auto* buffer = dynamic_cast<NullBuffer*>(binding.buffer);
					const RHI::BufferUsage required =
						declaration->type == RHI::ResourceBindingType::ConstantBuffer
						? RHI::BufferUsage::Constant
						: RHI::BufferUsage::Storage;
					if(buffer == nullptr || binding.texture != nullptr || binding.size == 0 ||
						!HasUsage(buffer->GetDesc().usage, required) ||
						binding.offset > buffer->GetDesc().size ||
						binding.size > buffer->GetDesc().size - binding.offset)
					{
						return false;
					}
					break;
				}
				case RHI::ResourceBindingType::SampledTexture:
				case RHI::ResourceBindingType::StorageTexture:
				{
					auto* texture = dynamic_cast<NullTexture*>(binding.texture);
					const RHI::TextureUsage required =
						declaration->type == RHI::ResourceBindingType::SampledTexture
						? RHI::TextureUsage::ShaderResource : RHI::TextureUsage::Storage;
					uint32_t firstMip = 0;
					uint32_t mipCount = 0;
					uint32_t firstLayer = 0;
					uint32_t layerCount = 0;
					if(texture == nullptr || binding.buffer != nullptr ||
						!HasUsage(texture->GetDesc().usage, required))
					{
						return false;
					}
					if(!ResolveBindingSubresources(
						texture, binding, declaration->type,
						firstMip, mipCount, firstLayer, layerCount)) return false;
					break;
				}
				default:
					return false;
				}
			}

			for(uint32_t index = 0; index < layout.bindingCount; ++index)
			{
				const RHI::ResourceBindingLayout& declaration = layout.bindings[index];
				if(declaration.type == RHI::ResourceBindingType::StaticSampler) continue;
				for(uint32_t element = 0; element < declaration.count; ++element)
				{
					if(populated.find({declaration.binding, element}) == populated.end())
						return false;
				}
			}
			return true;
		}

		class NullResourceSet final : public RHI::ResourceSet
		{
		public:
			explicit NullResourceSet(const RHI::ResourceSetDesc& desc)
				: RHI::ResourceSet(desc)
			{
			}
		};

		struct NullSubmissionState
		{
			std::unordered_map<NullBuffer*, RHI::ResourceState> buffers;
			std::map<std::pair<NullTexture*, uint32_t>, RHI::ResourceState> textures;
		};

		class NullCommandList final : public RHI::ICommandList
		{
		public:
			void ResourceBarrier(
				const RHI::ResourceBarrierDesc* barriers,
				uint32_t count) override
			{
				if(m_closed || m_rendering || (count != 0 && barriers == nullptr))
				{
					Invalidate();
					return;
				}

				for(uint32_t index = 0; index < count; ++index)
				{
					const RHI::ResourceBarrierDesc& barrier = barriers[index];
					if((barrier.buffer == nullptr) == (barrier.texture == nullptr) ||
						barrier.after == RHI::ResourceState::Undefined ||
						(barrier.before == barrier.after &&
							barrier.before != RHI::ResourceState::UnorderedAccess))
					{
						Invalidate();
						continue;
					}

					if(barrier.buffer != nullptr)
					{
						auto* buffer = dynamic_cast<NullBuffer*>(barrier.buffer);
						if(buffer == nullptr || !IsBufferStateAllowed(buffer->GetDesc(), barrier.after) ||
							(m_bufferStates.find(buffer) != m_bufferStates.end() &&
								m_bufferStates[buffer] != barrier.before))
						{
							Invalidate();
							continue;
						}
						Operation operation = {};
						operation.kind = OperationKind::BufferBarrier;
						operation.buffer = buffer;
						operation.before = barrier.before;
						operation.after = barrier.after;
						m_bufferStates[buffer] = barrier.after;
						m_operations.push_back(std::move(operation));
					}
					else
					{
						auto* texture = dynamic_cast<NullTexture*>(barrier.texture);
						uint32_t firstMip = 0;
						uint32_t mipCount = 0;
						uint32_t firstLayer = 0;
						uint32_t layerCount = 0;
						if(texture == nullptr ||
							!IsTextureStateAllowed(texture->GetDesc(), barrier.after) ||
							((barrier.before == RHI::ResourceState::Present ||
								barrier.after == RHI::ResourceState::Present) &&
								!texture->IsSwapchainImage()) ||
							!ResolveSubresources(
								texture, barrier.subresources,
								firstMip, mipCount, firstLayer, layerCount))
						{
							Invalidate();
							continue;
						}
						TrackSwapchainImage(texture);
						for(uint32_t layer = firstLayer; layer < firstLayer + layerCount; ++layer)
						{
							for(uint32_t mip = firstMip; mip < firstMip + mipCount; ++mip)
							{
								const auto key = TextureKey(texture, mip, layer);
								const auto prior = m_textureStates.find(key);
								if(prior != m_textureStates.end() && prior->second != barrier.before)
								{
									Invalidate();
									continue;
								}
								Operation operation = {};
								operation.kind = OperationKind::TextureBarrier;
								operation.texture = texture;
								operation.before = barrier.before;
								operation.after = barrier.after;
								operation.mipLevel = mip;
								operation.arrayLayer = layer;
								m_textureStates[key] = barrier.after;
								m_operations.push_back(std::move(operation));
							}
						}
					}
				}
			}

			void BeginRendering(const RHI::RenderingDesc& desc) override
			{
				if(m_closed || m_rendering ||
					(desc.colorAttachmentCount != 0 && desc.colorAttachments == nullptr) ||
					(desc.colorAttachmentCount == 0 && desc.depthStencilAttachment == nullptr))
				{
					Invalidate();
					return;
				}
				m_pipeline = nullptr;
				m_resourceSet = nullptr;
				m_vertexBindings.clear();
				m_indexBuffer = nullptr;
				m_indexFormat = RHI::Format::Unknown;
				m_indexOffset = 0;
				m_inlineConstants.clear();
				m_inlineConstantCoverage.clear();
				m_viewportSet = false;
				m_scissorSet = false;
				m_stencilReferenceSet = false;

				m_colorFormats.clear();
				for(uint32_t index = 0; index < desc.colorAttachmentCount; ++index)
				{
					const RHI::ColorAttachment& attachment = desc.colorAttachments[index];
					auto* texture = dynamic_cast<NullTexture*>(attachment.texture);
					bool clearValueValid = true;
					if(attachment.loadOp == RHI::LoadOp::Clear)
					{
						for(float component : attachment.clearColor)
							clearValueValid = clearValueValid && std::isfinite(component);
					}
					if(texture == nullptr || !HasUsage(texture->GetDesc().usage, RHI::TextureUsage::RenderTarget) ||
						attachment.mipLevel >= texture->GetDesc().mipLevels ||
						attachment.arrayLayer >= texture->GetDesc().depthOrArraySize ||
						!RequireTextureState(
							texture, attachment.mipLevel, attachment.arrayLayer,
							RHI::ResourceState::RenderTarget) ||
						attachment.loadOp == RHI::LoadOp::Undefined || !clearValueValid ||
						attachment.storeOp == RHI::StoreOp::Undefined)
					{
						Invalidate();
						return;
					}
					m_colorFormats.push_back(texture->GetDesc().format);
					TrackSwapchainImage(texture);
				}

				m_depthFormat = RHI::Format::Unknown;
				m_depthTexture = nullptr;
				m_depthMipLevel = 0;
				m_depthArrayLayer = 0;
				m_stencilConfigured = false;
				if(desc.depthStencilAttachment != nullptr)
				{
					const RHI::DepthStencilAttachment& attachment = *desc.depthStencilAttachment;
					auto* texture = dynamic_cast<NullTexture*>(attachment.texture);
					const bool subresourceValid = texture != nullptr &&
						attachment.mipLevel < texture->GetDesc().mipLevels &&
						attachment.arrayLayer < texture->GetDesc().depthOrArraySize;
					const bool hasStencil = texture != nullptr &&
						texture->GetDesc().format == RHI::Format::D24_UNORM_S8_UINT;
					const bool declaredStateValid =
						attachment.state == RHI::ResourceState::DepthRead ||
						attachment.state == RHI::ResourceState::DepthWrite;
					const bool requiresWrite = attachment.depthLoadOp == RHI::LoadOp::Clear ||
						(hasStencil && attachment.stencilLoadOp == RHI::LoadOp::Clear);
					const bool stateValid = subresourceValid && declaredStateValid &&
						(!requiresWrite || attachment.state == RHI::ResourceState::DepthWrite) &&
						RequireTextureState(
							texture, attachment.mipLevel, attachment.arrayLayer,
							attachment.state);
					const bool depthClearValid = attachment.depthLoadOp != RHI::LoadOp::Clear ||
						(std::isfinite(attachment.clearDepth) && attachment.clearDepth >= 0.0f &&
							attachment.clearDepth <= 1.0f);
					const bool stencilClearValid = attachment.stencilLoadOp != RHI::LoadOp::Clear ||
						attachment.clearStencil <= std::numeric_limits<uint8_t>::max();
					if(texture == nullptr || !HasUsage(texture->GetDesc().usage, RHI::TextureUsage::DepthStencil) ||
						!subresourceValid || !stateValid || !depthClearValid || !stencilClearValid ||
						attachment.depthLoadOp == RHI::LoadOp::Undefined ||
						attachment.depthStoreOp == RHI::StoreOp::Undefined ||
						(!hasStencil &&
							(attachment.stencilLoadOp != RHI::LoadOp::Undefined ||
								attachment.stencilStoreOp != RHI::StoreOp::Undefined)))
					{
						Invalidate();
						return;
					}
					m_depthFormat = texture->GetDesc().format;
					m_depthTexture = texture;
					m_depthMipLevel = attachment.mipLevel;
					m_depthArrayLayer = attachment.arrayLayer;
					if(hasStencil)
					{
						const bool hasStencilLoad =
							attachment.stencilLoadOp != RHI::LoadOp::Undefined;
						const bool hasStencilStore =
							attachment.stencilStoreOp != RHI::StoreOp::Undefined;
						if(hasStencilLoad != hasStencilStore)
						{
							Invalidate();
							return;
						}
						m_stencilConfigured = hasStencilLoad;
					}
				}

				m_rendering = true;
			}

			void EndRendering() override
			{
				if(m_closed || !m_rendering)
				{
					Invalidate();
					return;
				}
				m_rendering = false;
			}

			void BindGraphicsPipeline(RHI::PipelineHandle pipelineState) override
			{
				auto* pipeline = dynamic_cast<NullPipelineState*>(pipelineState);
				if(m_closed || !m_rendering || pipeline == nullptr)
				{
					Invalidate();
					return;
				}
				m_pipeline = pipeline;
				m_resourceSet = nullptr;
				m_inlineConstants.assign(pipeline->GetLayout().inlineConstantSize, 0);
				m_inlineConstantCoverage.assign(
					pipeline->GetLayout().inlineConstantSize, 0);
				if((pipeline->GetDesc().depthStencil.depthWriteEnabled ||
					pipeline->GetDesc().depthStencil.stencilEnabled) &&
					m_depthTexture != nullptr &&
					!RequireTextureState(
						m_depthTexture,
						m_depthMipLevel,
						m_depthArrayLayer,
						RHI::ResourceState::DepthWrite))
				{
					Invalidate();
				}
			}

			void BindResourceSet(RHI::ResourceSetHandle resourceSet) override
			{
				auto* set = dynamic_cast<NullResourceSet*>(resourceSet);
				if(m_closed || !m_rendering || m_pipeline == nullptr ||
					set == nullptr || set->GetPipeline() != m_pipeline)
				{
					Invalidate();
					return;
				}
				const RHI::PipelineLayoutDesc& layout = m_pipeline->GetLayout();
				for(uint32_t index = 0; index < set->GetBindingCount(); ++index)
				{
					const RHI::ResourceBinding& binding = set->GetBindings()[index];
					const RHI::ResourceBindingLayout* declaration =
						FindLayoutBinding(layout, binding.binding);
					if(declaration == nullptr)
					{
						Invalidate();
						return;
					}
					if(declaration->type == RHI::ResourceBindingType::ConstantBuffer ||
						declaration->type == RHI::ResourceBindingType::ReadOnlyStorageBuffer ||
						declaration->type == RHI::ResourceBindingType::ReadWriteStorageBuffer)
					{
						auto* buffer = dynamic_cast<NullBuffer*>(binding.buffer);
						const RHI::ResourceState requiredState =
							declaration->type == RHI::ResourceBindingType::ConstantBuffer
							? RHI::ResourceState::ConstantBuffer
							: declaration->type == RHI::ResourceBindingType::ReadOnlyStorageBuffer
								? RHI::ResourceState::ShaderResource
								: RHI::ResourceState::UnorderedAccess;
						if(buffer == nullptr ||
							!RequireBufferState(buffer, requiredState))
						{
							Invalidate();
							return;
						}
					}
					else if(declaration->type == RHI::ResourceBindingType::SampledTexture ||
						declaration->type == RHI::ResourceBindingType::StorageTexture)
					{
						auto* texture = dynamic_cast<NullTexture*>(binding.texture);
						const RHI::ResourceState required =
							declaration->type == RHI::ResourceBindingType::SampledTexture
							? RHI::ResourceState::ShaderResource
							: RHI::ResourceState::UnorderedAccess;
						uint32_t firstMip = 0;
						uint32_t mipCount = 0;
						uint32_t firstLayer = 0;
						uint32_t layerCount = 0;
						if(texture == nullptr || !ResolveBindingSubresources(
							texture, binding, declaration->type,
							firstMip, mipCount, firstLayer, layerCount) ||
							!RequireTextureSubresourcesInState(
								texture, firstMip, mipCount, firstLayer, layerCount, required))
						{
							Invalidate();
							return;
						}
					}
				}
				m_resourceSet = set;
			}

			void BindVertexBuffer(
				uint32_t binding,
				RHI::BufferHandle buffer,
				uint32_t offset) override
			{
				auto* nullBuffer = dynamic_cast<NullBuffer*>(buffer);
				const RHI::GraphicsPipelineDesc* pipelineDesc = m_pipeline == nullptr
					? nullptr : &m_pipeline->GetDesc();
				const RHI::VertexBufferLayout* layout = nullptr;
				if(pipelineDesc != nullptr)
				{
					for(uint32_t index = 0; index < pipelineDesc->vertexBufferCount; ++index)
					{
						if(pipelineDesc->vertexBuffers[index].binding == binding)
						{
							layout = &pipelineDesc->vertexBuffers[index];
							break;
						}
					}
				}
				if(m_closed || !m_rendering || pipelineDesc == nullptr || layout == nullptr ||
					nullBuffer == nullptr || offset >= nullBuffer->GetDesc().size ||
					!HasUsage(nullBuffer->GetDesc().usage, RHI::BufferUsage::Vertex) ||
					!RequireBufferState(
						nullBuffer, RHI::ResourceState::VertexBuffer))
				{
					Invalidate();
					return;
				}
				m_vertexBindings[binding] = {nullBuffer, offset};
			}

			void BindIndexBuffer(
				RHI::BufferHandle buffer,
				RHI::Format format,
				uint32_t offset) override
			{
				auto* nullBuffer = dynamic_cast<NullBuffer*>(buffer);
				const uint32_t indexSize = FormatSize(format);
				if(m_closed || !m_rendering || nullBuffer == nullptr ||
					offset >= nullBuffer->GetDesc().size ||
					!HasUsage(nullBuffer->GetDesc().usage, RHI::BufferUsage::Index) ||
					!RequireBufferState(
						nullBuffer, RHI::ResourceState::IndexBuffer) ||
					(format != RHI::Format::R16_UINT && format != RHI::Format::R32_UINT) ||
					indexSize == 0 || offset % indexSize != 0 ||
					nullBuffer->GetDesc().size - offset < indexSize)
				{
					Invalidate();
					return;
				}
				m_indexBuffer = nullBuffer;
				m_indexFormat = format;
				m_indexOffset = offset;
			}

			void SetInlineConstants(
				uint32_t offset,
				uint32_t size,
				const void* data) override
			{
				if(m_closed || !m_rendering || m_pipeline == nullptr ||
					data == nullptr || size == 0 ||
					(offset % sizeof(uint32_t)) != 0 ||
					(size % sizeof(uint32_t)) != 0 ||
					offset > m_inlineConstants.size() || size > m_inlineConstants.size() - offset)
				{
					Invalidate();
					return;
				}
				std::memcpy(m_inlineConstants.data() + offset, data, size);
				std::fill(
					m_inlineConstantCoverage.begin() + offset,
					m_inlineConstantCoverage.begin() + offset + size,
					1);
			}

			void SetViewport(const RHI::Viewport& viewport) override
			{
				if(m_closed || !m_rendering ||
					!std::isfinite(viewport.x) || !std::isfinite(viewport.y) ||
					!std::isfinite(viewport.width) || !std::isfinite(viewport.height) ||
					!std::isfinite(viewport.minDepth) || !std::isfinite(viewport.maxDepth) ||
					viewport.width <= 0.0f || viewport.height <= 0.0f ||
					viewport.minDepth < 0.0f || viewport.maxDepth > 1.0f ||
					viewport.minDepth > viewport.maxDepth)
				{
					Invalidate();
					return;
				}
				m_viewportSet = true;
			}

			void SetScissor(const RHI::Rect& rect) override
			{
				if(m_closed || !m_rendering || rect.x < 0 || rect.y < 0 ||
					rect.width == 0 || rect.height == 0)
				{
					Invalidate();
					return;
				}
				m_scissorSet = true;
			}

			void SetStencilReference(uint32_t reference) override
			{
				if(m_closed || !m_rendering ||
					reference > std::numeric_limits<uint8_t>::max())
				{
					Invalidate();
					return;
				}
				m_stencilReferenceSet = true;
			}

			void DrawInstanced(
				uint32_t vertexCount,
				uint32_t instanceCount,
				uint32_t startVertex,
				uint32_t startInstance) override
			{
				if(vertexCount == 0 || instanceCount == 0 ||
					!ValidateDraw(false, vertexCount, instanceCount, startVertex, startInstance))
				{
					Invalidate();
				}
			}

			void DrawIndexedInstanced(
				uint32_t indexCount,
				uint32_t instanceCount,
				uint32_t firstIndex,
				int32_t,
				uint32_t firstInstance) override
			{
				const uint32_t indexSize = FormatSize(m_indexFormat);
				const uint64_t firstByte = static_cast<uint64_t>(m_indexOffset) +
					static_cast<uint64_t>(firstIndex) * indexSize;
				const uint64_t end = firstByte +
					static_cast<uint64_t>(indexCount) * indexSize;
				if(indexCount == 0 || instanceCount == 0 ||
					!ValidateDraw(true, 0, instanceCount, 0, firstInstance) ||
					m_indexBuffer == nullptr || end > m_indexBuffer->GetDesc().size)
				{
					Invalidate();
				}
			}

			void Close() override
			{
				if(m_closed || m_rendering) Invalidate();
				m_closed = true;
			}

			[[nodiscard]] bool RecordBufferUpdate(
				NullBuffer* buffer,
				uint32_t offset,
				const void* data,
				uint32_t size)
			{
				if(m_closed || m_rendering || buffer == nullptr || data == nullptr || size == 0 ||
					offset > buffer->GetDesc().size || size > buffer->GetDesc().size - offset)
				{
					return false;
				}
				Operation operation = {};
				operation.kind = OperationKind::BufferWrite;
				operation.buffer = buffer;
				operation.offset = offset;
				operation.data.resize(size);
				std::memcpy(operation.data.data(), data, size);
				m_operations.push_back(std::move(operation));
				return true;
			}

			[[nodiscard]] bool RecordTextureUpdate(
				NullTexture* texture,
				uint32_t mipLevel,
				uint32_t arrayLayer,
				const void* data,
				uint32_t dataSize,
				uint32_t rowPitch,
				uint32_t slicePitch)
			{
				if(m_closed || m_rendering || texture == nullptr || data == nullptr ||
					mipLevel >= texture->GetDesc().mipLevels ||
					arrayLayer >= texture->GetDesc().depthOrArraySize ||
					texture->GetDesc().width == 0 || texture->GetDesc().height == 0)
				{
					return false;
				}
				const uint32_t pixelSize = FormatSize(texture->GetDesc().format);
				const uint32_t mipWidth = std::max(1u, texture->GetDesc().width >> mipLevel);
				const uint32_t mipHeight = std::max(1u, texture->GetDesc().height >> mipLevel);
				if(pixelSize == 0 || mipWidth >
					std::numeric_limits<uint32_t>::max() / pixelSize ||
					rowPitch < mipWidth * pixelSize ||
					mipHeight > std::numeric_limits<uint32_t>::max() / rowPitch ||
					slicePitch < rowPitch * mipHeight || dataSize < slicePitch)
				{
					return false;
				}
				Operation operation = {};
				operation.kind = OperationKind::TextureWrite;
				operation.texture = texture;
				operation.mipLevel = mipLevel;
				operation.arrayLayer = arrayLayer;
				operation.rowPitch = rowPitch;
				operation.slicePitch = slicePitch;
				operation.data.resize(dataSize);
				std::memcpy(operation.data.data(), data, operation.data.size());
				m_operations.push_back(std::move(operation));
				TrackSwapchainImage(texture);
				return true;
			}

			[[nodiscard]] bool IsClosed() const { return m_closed; }
			[[nodiscard]] bool IsValid() const { return m_valid; }
			[[nodiscard]] const std::vector<NullTexture*>& GetReferencedSwapchainImages() const
			{
				return m_referencedSwapchainImages;
			}

			[[nodiscard]] bool ValidateForSubmit(NullSubmissionState& state) const
			{
				for(const Operation& operation : m_operations)
				{
					switch(operation.kind)
					{
					case OperationKind::BufferBarrier:
					{
						const auto found = state.buffers.find(operation.buffer);
						const RHI::ResourceState current = found == state.buffers.end()
							? operation.buffer->GetState() : found->second;
						if(current != operation.before) return false;
						state.buffers[operation.buffer] = operation.after;
						break;
					}
					case OperationKind::TextureBarrier:
					{
						const auto key = TextureKey(
							operation.texture, operation.mipLevel, operation.arrayLayer);
						const auto found = state.textures.find(key);
						const RHI::ResourceState current = found == state.textures.end()
							? operation.texture->GetState(
								operation.mipLevel, operation.arrayLayer) : found->second;
						if(current != operation.before) return false;
						state.textures[key] = operation.after;
						break;
					}
					case OperationKind::BufferRequirement:
					{
						const auto found = state.buffers.find(operation.buffer);
						const RHI::ResourceState current = found == state.buffers.end()
							? operation.buffer->GetState() : found->second;
						if(current != operation.before &&
							(operation.after == RHI::ResourceState::Undefined ||
								current != operation.after))
							return false;
						break;
					}
					case OperationKind::TextureRequirement:
					{
						const auto key = TextureKey(
							operation.texture, operation.mipLevel, operation.arrayLayer);
						const auto found = state.textures.find(key);
						const RHI::ResourceState current = found == state.textures.end()
							? operation.texture->GetState(
								operation.mipLevel, operation.arrayLayer) : found->second;
						if(current != operation.before &&
							(operation.after == RHI::ResourceState::Undefined ||
								current != operation.after))
							return false;
						break;
					}
					case OperationKind::BufferWrite:
					{
						const auto found = state.buffers.find(operation.buffer);
						const RHI::ResourceState current = found == state.buffers.end()
							? operation.buffer->GetState() : found->second;
						if(current != RHI::ResourceState::CopyDestination) return false;
						break;
					}
					case OperationKind::TextureWrite:
					{
						const auto key = TextureKey(
							operation.texture, operation.mipLevel, operation.arrayLayer);
						const auto found = state.textures.find(key);
						const RHI::ResourceState current = found == state.textures.end()
							? operation.texture->GetState(
								operation.mipLevel, operation.arrayLayer) : found->second;
						if(current != RHI::ResourceState::CopyDestination) return false;
						break;
					}
					}
				}
				return true;
			}

			void Apply()
			{
				for(const Operation& operation : m_operations)
				{
					switch(operation.kind)
					{
					case OperationKind::BufferBarrier:
						operation.buffer->SetState(operation.after);
						break;
					case OperationKind::TextureBarrier:
						operation.texture->SetState(
							operation.mipLevel, operation.arrayLayer, operation.after);
						break;
					case OperationKind::BufferRequirement:
					case OperationKind::TextureRequirement:
						break;
					case OperationKind::BufferWrite:
						operation.buffer->Write(operation.offset, operation.data);
						break;
					case OperationKind::TextureWrite:
						operation.texture->Write(
							operation.mipLevel,
							operation.arrayLayer,
							operation.rowPitch,
							operation.slicePitch,
							operation.data);
						break;
					}
				}
			}

		private:
			enum class OperationKind : uint8_t
			{
				BufferBarrier,
				TextureBarrier,
				BufferRequirement,
				TextureRequirement,
				BufferWrite,
				TextureWrite
			};

			struct Operation
			{
				OperationKind kind = OperationKind::BufferBarrier;
				NullBuffer* buffer = nullptr;
				NullTexture* texture = nullptr;
				RHI::ResourceState before = RHI::ResourceState::Undefined;
				RHI::ResourceState after = RHI::ResourceState::Undefined;
				uint32_t offset = 0;
				uint32_t mipLevel = 0;
				uint32_t arrayLayer = 0;
				uint32_t rowPitch = 0;
				uint32_t slicePitch = 0;
				std::vector<uint8_t> data;
			};

			struct VertexBinding
			{
				NullBuffer* buffer = nullptr;
				uint32_t offset = 0;
			};

			void Invalidate() { m_valid = false; }

			[[nodiscard]] bool RequireBufferState(
				NullBuffer* buffer,
				RHI::ResourceState first,
				RHI::ResourceState second = RHI::ResourceState::Undefined)
			{
				const auto found = m_bufferStates.find(buffer);
				if(found != m_bufferStates.end() &&
					found->second != first &&
					(second == RHI::ResourceState::Undefined || found->second != second))
				{
					return false;
				}
				Operation operation = {};
				operation.kind = OperationKind::BufferRequirement;
				operation.buffer = buffer;
				operation.before = first;
				operation.after = second;
				m_operations.push_back(std::move(operation));
				return true;
			}

			[[nodiscard]] bool RequireTextureState(
				NullTexture* texture,
				uint32_t mipLevel,
				uint32_t arrayLayer,
				RHI::ResourceState first,
				RHI::ResourceState second = RHI::ResourceState::Undefined)
			{
				const auto key = TextureKey(texture, mipLevel, arrayLayer);
				const auto found = m_textureStates.find(key);
				if(found != m_textureStates.end() &&
					found->second != first &&
					(second == RHI::ResourceState::Undefined || found->second != second))
				{
					return false;
				}
				Operation operation = {};
				operation.kind = OperationKind::TextureRequirement;
				operation.texture = texture;
				operation.mipLevel = mipLevel;
				operation.arrayLayer = arrayLayer;
				operation.before = first;
				operation.after = second;
				m_operations.push_back(std::move(operation));
				return true;
			}

			void TrackSwapchainImage(NullTexture* texture)
			{
				if(texture == nullptr || !texture->IsSwapchainImage()) return;
				if(std::find(m_referencedSwapchainImages.begin(),
					m_referencedSwapchainImages.end(), texture) ==
					m_referencedSwapchainImages.end())
				{
					m_referencedSwapchainImages.push_back(texture);
				}
			}

			[[nodiscard]] bool RequireTextureSubresourcesInState(
				NullTexture* texture,
				uint32_t firstMip,
				uint32_t mipCount,
				uint32_t firstLayer,
				uint32_t layerCount,
				RHI::ResourceState state)
			{
				for(uint32_t layer = firstLayer; layer < firstLayer + layerCount; ++layer)
					for(uint32_t mip = firstMip; mip < firstMip + mipCount; ++mip)
						if(!RequireTextureState(
							texture, mip, layer, state)) return false;
				return true;
			}

			[[nodiscard]] bool ValidateDraw(
				bool indexed,
				uint32_t vertexCount,
				uint32_t instanceCount,
				uint32_t startVertex,
				uint32_t startInstance) const
			{
				if(m_closed || !m_rendering || m_pipeline == nullptr ||
					!m_viewportSet || !m_scissorSet ||
					(indexed && m_indexBuffer == nullptr))
				{
					return false;
				}

				const RHI::GraphicsPipelineDesc& desc = m_pipeline->GetDesc();
				if(desc.colorAttachmentCount != m_colorFormats.size() ||
					desc.depthStencil.format != m_depthFormat)
				{
					return false;
				}
				if(desc.depthStencil.stencilEnabled &&
					(!m_stencilConfigured || !m_stencilReferenceSet))
				{
					return false;
				}
				for(uint32_t index = 0; index < desc.colorAttachmentCount; ++index)
				{
					if(desc.colorAttachments[index].format != m_colorFormats[index]) return false;
				}
				for(uint32_t index = 0; index < desc.vertexBufferCount; ++index)
				{
					const RHI::VertexBufferLayout& layout = desc.vertexBuffers[index];
					const auto found = m_vertexBindings.find(layout.binding);
					if(found == m_vertexBindings.end())
					{
						return false;
					}
					uint64_t first = 0;
					uint64_t count = 0;
					if(layout.stepMode == RHI::VertexStepMode::Instance)
					{
						first = startInstance;
						count = instanceCount;
					}
					else if(!indexed)
					{
						first = startVertex;
						count = vertexCount;
					}
					const uint64_t requiredEnd = static_cast<uint64_t>(found->second.offset) +
						(first + count) * layout.stride;
					if(requiredEnd > found->second.buffer->GetDesc().size) return false;
				}
				bool resourceSetRequired = false;
				for(uint32_t index = 0; index < desc.layout.bindingCount; ++index)
				{
					if(desc.layout.bindings[index].type !=
						RHI::ResourceBindingType::StaticSampler)
					{
						resourceSetRequired = true;
						break;
					}
				}
				if(resourceSetRequired && m_resourceSet == nullptr) return false;
				if(desc.layout.inlineConstantSize != 0 &&
					(m_inlineConstantCoverage.size() != desc.layout.inlineConstantSize ||
						std::find(m_inlineConstantCoverage.begin(),
							m_inlineConstantCoverage.end(), 0) !=
							m_inlineConstantCoverage.end()))
				{
					return false;
				}
				return true;
			}

			std::vector<Operation> m_operations;
			std::unordered_map<NullBuffer*, RHI::ResourceState> m_bufferStates;
			std::map<std::pair<NullTexture*, uint32_t>, RHI::ResourceState> m_textureStates;
			std::unordered_map<uint32_t, VertexBinding> m_vertexBindings;
			std::vector<NullTexture*> m_referencedSwapchainImages;
			std::vector<RHI::Format> m_colorFormats;
			std::vector<uint8_t> m_inlineConstants;
			std::vector<uint8_t> m_inlineConstantCoverage;
			NullPipelineState* m_pipeline = nullptr;
			NullResourceSet* m_resourceSet = nullptr;
			NullBuffer* m_indexBuffer = nullptr;
			NullTexture* m_depthTexture = nullptr;
			RHI::Format m_indexFormat = RHI::Format::Unknown;
			RHI::Format m_depthFormat = RHI::Format::Unknown;
			uint32_t m_depthMipLevel = 0;
			uint32_t m_depthArrayLayer = 0;
			uint32_t m_indexOffset = 0;
			bool m_rendering = false;
			bool m_stencilConfigured = false;
			bool m_viewportSet = false;
			bool m_scissorSet = false;
			bool m_stencilReferenceSet = false;
			bool m_closed = false;
			bool m_valid = true;
		};

		struct NullFrameSlot
		{
			uint64_t completionValue = 0;
		};

		template<typename Object>
		struct RetiredObject
		{
			uint64_t completionValue = 0;
			std::unique_ptr<Object> object;
		};

		template<typename Object, typename Interface>
		bool RetireObject(
			std::vector<std::unique_ptr<Object>>& liveObjects,
			Interface* object,
			uint64_t completionValue,
			std::vector<RetiredObject<Object>>& retiredObjects)
		{
			const auto found = std::find_if(
				liveObjects.begin(), liveObjects.end(),
				[object](const std::unique_ptr<Object>& candidate)
				{
					return static_cast<Interface*>(candidate.get()) == object;
				});
			if(found == liveObjects.end()) return false;
			retiredObjects.push_back({completionValue, nullptr});
			retiredObjects.back().object = std::move(*found);
			liveObjects.erase(found);
			return true;
		}

		template<typename Object>
		void ReclaimObjects(
			std::vector<RetiredObject<Object>>& objects,
			uint64_t completedValue)
		{
			objects.erase(
				std::remove_if(
					objects.begin(), objects.end(),
					[completedValue](const RetiredObject<Object>& object)
					{
						return object.completionValue <= completedValue;
					}),
				objects.end());
		}
	}

	struct NullDevice::Impl
	{
		const void* windowHandle = nullptr;
		std::vector<std::unique_ptr<NullTexture>> backBuffers;
		std::vector<uint64_t> imageCompletionValues;
		std::vector<NullFrameSlot> frames;
		std::vector<std::unique_ptr<NullCommandList>> activeCommandLists;
		std::vector<std::unique_ptr<NullBuffer>> liveBuffers;
		std::vector<std::unique_ptr<NullTexture>> liveTextures;
		std::vector<std::unique_ptr<NullShader>> liveShaders;
		std::vector<std::unique_ptr<NullPipelineState>> livePipelines;
		std::vector<std::unique_ptr<NullResourceSet>> liveResourceSets;
		std::vector<RetiredObject<NullBuffer>> retiredBuffers;
		std::vector<RetiredObject<NullTexture>> retiredTextures;
		std::vector<RetiredObject<NullShader>> retiredShaders;
		std::vector<RetiredObject<NullPipelineState>> retiredPipelines;
		std::vector<RetiredObject<NullResourceSet>> retiredResourceSets;
		uint64_t nextCompletionValue = 1;
		uint64_t completedValue = 0;
		uint64_t lastSubmittedValue = 0;
		uint32_t nextFrameIndex = 0;
		uint32_t activeFrameIndex = 0;
		uint32_t activeImageIndex = 0;
		uint32_t nextImageIndex = 0;
		bool initialized = false;
		bool swapchainReady = false;
		bool frameReady = false;
		bool frameSubmitted = false;

		void CollectRetiredObjects()
		{
			ReclaimObjects(retiredResourceSets, completedValue);
			ReclaimObjects(retiredPipelines, completedValue);
			ReclaimObjects(retiredShaders, completedValue);
			ReclaimObjects(retiredTextures, completedValue);
			ReclaimObjects(retiredBuffers, completedValue);
		}
	};

	NullDevice::NullDevice()
		: m_impl(new Impl())
	{
	}

	NullDevice::~NullDevice()
	{
		m_impl->activeCommandLists.clear();
		m_impl->retiredResourceSets.clear();
		m_impl->liveResourceSets.clear();
		m_impl->retiredPipelines.clear();
		m_impl->livePipelines.clear();
		m_impl->retiredShaders.clear();
		m_impl->liveShaders.clear();
		m_impl->retiredTextures.clear();
		m_impl->liveTextures.clear();
		m_impl->retiredBuffers.clear();
		m_impl->liveBuffers.clear();
		delete m_impl;
	}

	bool NullDevice::CreateSwapchain(const RHI::SwapchainDesc& desc)
	{
		if(m_impl == nullptr || !m_impl->initialized || m_impl->swapchainReady ||
			desc.minimumImageCount == 0 || desc.initialWidth == 0 || desc.initialHeight == 0)
		{
			return false;
		}

		RHI::Format format = desc.format;
		if(format == RHI::Format::Unknown) format = RHI::Format::R8G8B8A8_UNORM;
		switch(desc.presentMode)
		{
		case RHI::PresentMode::Immediate:
		case RHI::PresentMode::Mailbox:
		case RHI::PresentMode::Fifo:
			break;
		default:
			return false;
		}
		if(!IsColorFormat(format)) return false;

		RHI::TextureDesc backBufferDesc = {};
		backBufferDesc.width = desc.initialWidth;
		backBufferDesc.height = desc.initialHeight;
		backBufferDesc.format = format;
		backBufferDesc.usage = RHI::TextureUsage::RenderTarget;

		std::vector<std::unique_ptr<NullTexture>> backBuffers;
		backBuffers.reserve(desc.minimumImageCount);
		for(uint32_t index = 0; index < desc.minimumImageCount; ++index)
		{
			auto backBuffer = std::make_unique<NullTexture>(backBufferDesc, true);
			backBuffers.push_back(std::move(backBuffer));
		}

		m_impl->backBuffers = std::move(backBuffers);
		m_impl->imageCompletionValues.assign(desc.minimumImageCount, 0);
		m_impl->nextImageIndex = 0;
		m_impl->swapchainReady = true;
		return true;
	}

	bool NullDevice::BeginFrame()
	{
		if(m_impl == nullptr || !m_impl->swapchainReady || m_impl->frameSubmitted ||
			m_impl->frames.empty() || m_impl->backBuffers.empty())
		{
			return false;
		}
		if(m_impl->frameReady)
		{
			return m_impl->activeFrameIndex < m_impl->frames.size() &&
				m_impl->activeImageIndex < m_impl->backBuffers.size();
		}

		const uint32_t frameIndex = m_impl->nextFrameIndex;
		const uint32_t imageIndex = m_impl->nextImageIndex;
		if(m_impl->frames[frameIndex].completionValue > m_impl->completedValue ||
			m_impl->imageCompletionValues[imageIndex] > m_impl->completedValue)
		{
			return false;
		}

		m_impl->activeFrameIndex = frameIndex;
		m_impl->activeImageIndex = imageIndex;
		m_impl->frameReady = true;
		return true;
	}

	RHI::ICommandList* NullDevice::AcquireCommandList()
	{
		if(m_impl == nullptr || !m_impl->initialized) return nullptr;
		auto commandList = std::make_unique<NullCommandList>();
		NullCommandList* result = commandList.get();
		m_impl->activeCommandLists.push_back(std::move(commandList));
		return result;
	}

	bool NullDevice::Submit(RHI::ICommandList** cmdLists, uint32_t count)
	{
		if(m_impl == nullptr || cmdLists == nullptr || count == 0)
			return false;

		NullTexture* activeBackBuffer = nullptr;
		if(m_impl->frameReady && m_impl->activeImageIndex < m_impl->backBuffers.size())
			activeBackBuffer = m_impl->backBuffers[m_impl->activeImageIndex].get();

		std::vector<NullCommandList*> submitted;
		submitted.reserve(count);
		for(uint32_t index = 0; index < count; ++index)
		{
			if(cmdLists[index] == nullptr) return false;
			for(uint32_t previous = 0; previous < index; ++previous)
				if(cmdLists[previous] == cmdLists[index]) return false;

			const auto owned = std::find_if(
				m_impl->activeCommandLists.begin(), m_impl->activeCommandLists.end(),
				[command = cmdLists[index]](const std::unique_ptr<NullCommandList>& candidate)
				{
					return candidate.get() == command;
				});
			if(owned == m_impl->activeCommandLists.end() || !(*owned)->IsClosed()) return false;
			submitted.push_back(owned->get());
		}

		std::vector<std::unique_ptr<NullCommandList>> consumed;
		consumed.reserve(count);
		for(NullCommandList* commandList : submitted)
		{
			const auto owned = std::find_if(
				m_impl->activeCommandLists.begin(), m_impl->activeCommandLists.end(),
				[commandList](const std::unique_ptr<NullCommandList>& candidate)
				{
					return candidate.get() == commandList;
				});
			consumed.push_back(std::move(*owned));
			m_impl->activeCommandLists.erase(owned);
		}

		bool frameSubmission = false;
		for(NullCommandList* commandList : submitted)
		{
			if(!commandList->IsValid()) return false;
			for(NullTexture* image : commandList->GetReferencedSwapchainImages())
			{
				if(activeBackBuffer == nullptr || image != activeBackBuffer) return false;
				frameSubmission = true;
			}
		}
		if(!m_impl->initialized) return false;

		NullSubmissionState state = {};
		for(NullCommandList* commandList : submitted)
			if(!commandList->ValidateForSubmit(state)) return false;
		if(frameSubmission)
		{
			const auto found = state.textures.find(TextureKey(activeBackBuffer, 0, 0));
			const RHI::ResourceState finalState = found == state.textures.end()
				? activeBackBuffer->GetState(0, 0) : found->second;
			if(finalState != RHI::ResourceState::Present) return false;
		}

		for(NullCommandList* commandList : submitted) commandList->Apply();
		const uint64_t completionValue = m_impl->nextCompletionValue++;
		m_impl->completedValue = completionValue;
		m_impl->lastSubmittedValue = completionValue;
		m_impl->CollectRetiredObjects();
		if(frameSubmission)
		{
			m_impl->frames[m_impl->activeFrameIndex].completionValue = completionValue;
			m_impl->imageCompletionValues[m_impl->activeImageIndex] = completionValue;
			m_impl->nextFrameIndex = (m_impl->activeFrameIndex + 1) %
				static_cast<uint32_t>(m_impl->frames.size());
			m_impl->frameReady = false;
			m_impl->frameSubmitted = true;
		}
		return true;
	}

	void NullDevice::Present()
	{
		if(m_impl == nullptr || !m_impl->frameSubmitted) return;
		m_impl->nextImageIndex = (m_impl->activeImageIndex + 1) %
			static_cast<uint32_t>(m_impl->backBuffers.size());
		m_impl->frameSubmitted = false;
	}

	RHI::BufferHandle NullDevice::CreateBuffer(const RHI::BufferDesc& desc)
	{
		if(desc.size == 0 || desc.usage == RHI::BufferUsage::None ||
			!IsBufferStateAllowed(desc, desc.initialState))
		{
			return nullptr;
		}
		auto buffer = std::make_unique<NullBuffer>(desc);
		NullBuffer* result = buffer.get();
		m_impl->liveBuffers.push_back(std::move(buffer));
		return result;
	}

	RHI::TextureHandle NullDevice::CreateTexture(const RHI::TextureDesc& desc)
	{
		if(desc.width == 0 || desc.height == 0 || desc.depthOrArraySize == 0 ||
			desc.mipLevels == 0 || desc.mipLevels > MaximumMipCount(desc.width, desc.height) ||
			desc.format == RHI::Format::Unknown || desc.usage == RHI::TextureUsage::None ||
			(IsDepthFormat(desc.format) && HasUsage(desc.usage, RHI::TextureUsage::RenderTarget)) ||
			(!IsDepthFormat(desc.format) && HasUsage(desc.usage, RHI::TextureUsage::DepthStencil)))
		{
			return nullptr;
		}
		auto texture = std::make_unique<NullTexture>(desc);
		NullTexture* result = texture.get();
		m_impl->liveTextures.push_back(std::move(texture));
		return result;
	}

	RHI::ShaderHandle NullDevice::CreateShader(const RHI::ShaderDesc& desc)
	{
		if(desc.stage == RHI::ShaderStage::Unknown || desc.entryPoint == nullptr ||
			desc.entryPoint[0] == '\0' || desc.binary == nullptr || desc.binarySize == 0)
		{
			return nullptr;
		}
		auto shader = std::make_unique<NullShader>(desc);
		NullShader* result = shader.get();
		m_impl->liveShaders.push_back(std::move(shader));
		return result;
	}

	RHI::PipelineHandle NullDevice::CreateGraphicsPipeline(
		const RHI::GraphicsPipelineDesc& desc)
	{
		const auto ownsShader = [this](RHI::ShaderHandle shader)
		{
			return std::find_if(
				m_impl->liveShaders.begin(), m_impl->liveShaders.end(),
				[shader](const std::unique_ptr<NullShader>& candidate)
				{
					return static_cast<RHI::ShaderHandle>(candidate.get()) == shader;
				}) != m_impl->liveShaders.end();
		};
		if(!ownsShader(desc.vertexShader) ||
			(desc.fragmentShader != nullptr && !ownsShader(desc.fragmentShader)) ||
			!ValidateGraphicsPipelineDesc(desc))
		{
			return nullptr;
		}
		auto pipeline = std::make_unique<NullPipelineState>(desc);
		NullPipelineState* result = pipeline.get();
		m_impl->livePipelines.push_back(std::move(pipeline));
		return result;
	}

	RHI::ResourceSetHandle NullDevice::CreateResourceSet(const RHI::ResourceSetDesc& desc)
	{
		const auto pipeline = std::find_if(
			m_impl->livePipelines.begin(), m_impl->livePipelines.end(),
			[requested = desc.pipeline](const std::unique_ptr<NullPipelineState>& candidate)
			{
				return static_cast<RHI::PipelineHandle>(candidate.get()) == requested;
			});
		if(pipeline == m_impl->livePipelines.end() ||
			(desc.bindingCount != 0 && desc.bindings == nullptr))
		{
			return nullptr;
		}
		for(uint32_t index = 0; index < desc.bindingCount; ++index)
		{
			const RHI::ResourceBinding& binding = desc.bindings[index];
			if(binding.buffer != nullptr)
			{
				const auto buffer = std::find_if(
					m_impl->liveBuffers.begin(), m_impl->liveBuffers.end(),
					[requested = binding.buffer](const std::unique_ptr<NullBuffer>& candidate)
					{
						return static_cast<RHI::BufferHandle>(candidate.get()) == requested;
					});
				if(buffer == m_impl->liveBuffers.end()) return nullptr;
			}
			if(binding.texture != nullptr)
			{
				const auto texture = std::find_if(
					m_impl->liveTextures.begin(), m_impl->liveTextures.end(),
					[requested = binding.texture](const std::unique_ptr<NullTexture>& candidate)
					{
						return static_cast<RHI::TextureHandle>(candidate.get()) == requested;
					});
				if(texture == m_impl->liveTextures.end()) return nullptr;
			}
		}
		if(!ValidateResourceSetDesc(desc)) return nullptr;
		auto resourceSet = std::make_unique<NullResourceSet>(desc);
		NullResourceSet* result = resourceSet.get();
		m_impl->liveResourceSets.push_back(std::move(resourceSet));
		return result;
	}

	void NullDevice::DestroyBuffer(RHI::BufferHandle buffer)
	{
		if(RetireObject(
			m_impl->liveBuffers,
			buffer,
			m_impl->lastSubmittedValue,
			m_impl->retiredBuffers))
		{
			m_impl->CollectRetiredObjects();
		}
	}

	void NullDevice::DestroyTexture(RHI::TextureHandle texture)
	{
		if(RetireObject(
			m_impl->liveTextures,
			texture,
			m_impl->lastSubmittedValue,
			m_impl->retiredTextures))
		{
			m_impl->CollectRetiredObjects();
		}
	}

	void NullDevice::DestroyShader(RHI::ShaderHandle shader)
	{
		if(RetireObject(
			m_impl->liveShaders,
			shader,
			m_impl->lastSubmittedValue,
			m_impl->retiredShaders))
		{
			m_impl->CollectRetiredObjects();
		}
	}

	void NullDevice::DestroyPipeline(RHI::PipelineHandle pipeline)
	{
		if(RetireObject(
			m_impl->livePipelines,
			pipeline,
			m_impl->lastSubmittedValue,
			m_impl->retiredPipelines))
		{
			m_impl->CollectRetiredObjects();
		}
	}

	void NullDevice::DestroyResourceSet(RHI::ResourceSetHandle resourceSet)
	{
		if(RetireObject(
			m_impl->liveResourceSets,
			resourceSet,
			m_impl->lastSubmittedValue,
			m_impl->retiredResourceSets))
		{
			m_impl->CollectRetiredObjects();
		}
	}

	bool NullDevice::UpdateBuffer(
		RHI::ICommandList& commandList,
		RHI::BufferHandle buffer,
		uint32_t offset,
		const void* data,
		uint32_t size)
	{
		if(m_impl == nullptr) return false;
		const auto ownedCommandList = std::find_if(
			m_impl->activeCommandLists.begin(), m_impl->activeCommandLists.end(),
			[requested = &commandList](const std::unique_ptr<NullCommandList>& candidate)
			{
				return static_cast<RHI::ICommandList*>(candidate.get()) == requested;
			});
		const auto ownedBuffer = std::find_if(
			m_impl->liveBuffers.begin(), m_impl->liveBuffers.end(),
			[buffer](const std::unique_ptr<NullBuffer>& candidate)
			{
				return static_cast<RHI::BufferHandle>(candidate.get()) == buffer;
			});
		return ownedCommandList != m_impl->activeCommandLists.end() &&
			ownedBuffer != m_impl->liveBuffers.end() &&
			(*ownedCommandList)->RecordBufferUpdate(
				ownedBuffer->get(), offset, data, size);
	}

	bool NullDevice::UpdateTexture(
		RHI::ICommandList& commandList,
		RHI::TextureHandle texture,
		uint32_t mipLevel,
		uint32_t arrayLayer,
		const void* data,
		uint32_t dataSize,
		uint32_t rowPitch,
		uint32_t slicePitch)
	{
		if(m_impl == nullptr) return false;
		const auto ownedCommandList = std::find_if(
			m_impl->activeCommandLists.begin(), m_impl->activeCommandLists.end(),
			[requested = &commandList](const std::unique_ptr<NullCommandList>& candidate)
			{
				return static_cast<RHI::ICommandList*>(candidate.get()) == requested;
			});
		const auto ownedTexture = std::find_if(
			m_impl->liveTextures.begin(), m_impl->liveTextures.end(),
			[texture](const std::unique_ptr<NullTexture>& candidate)
			{
				return static_cast<RHI::TextureHandle>(candidate.get()) == texture;
			});
		return ownedCommandList != m_impl->activeCommandLists.end() &&
			ownedTexture != m_impl->liveTextures.end() &&
			(*ownedCommandList)->RecordTextureUpdate(
				ownedTexture->get(),
				mipLevel,
				arrayLayer,
				data,
				dataSize,
				rowPitch,
				slicePitch);
	}

	RHI::TextureHandle NullDevice::GetBackBuffer()
	{
		if(m_impl == nullptr || !m_impl->swapchainReady || m_impl->backBuffers.empty())
			return nullptr;
		const uint32_t imageIndex = (m_impl->frameReady || m_impl->frameSubmitted)
			? m_impl->activeImageIndex : m_impl->nextImageIndex;
		return imageIndex < m_impl->backBuffers.size()
			? m_impl->backBuffers[imageIndex].get() : nullptr;
	}

	int NullDevice::Initialize(const void* windowHandle, const RHI::DeviceDesc& desc)
	{
		if(m_impl == nullptr || windowHandle == nullptr || desc.maxFramesInFlight == 0)
			return -1;
		m_impl->windowHandle = windowHandle;
		m_impl->frames.resize(desc.maxFramesInFlight);
		m_impl->initialized = true;
		return 0;
	}
}
