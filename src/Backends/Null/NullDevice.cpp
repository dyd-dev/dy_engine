#include "NullDevice.h"
#include "NullValidation.h"

#include "dyf/RHI/Buffer.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/Pipeline.h"
#include "dyf/RHI/ResourceSet.h"
#include "dyf/RHI/Shader.h"
#include "dyf/RHI/Texture.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace dyf::Backends
{
	namespace
	{

























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

		private:
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

			explicit NullPipelineState(const RHI::MeshPipelineDesc& desc)
				: RHI::Pipeline(desc.layout, false, true)
			{
				if(desc.colorAttachmentCount != 0)
					m_colorAttachments.assign(
						desc.colorAttachments, desc.colorAttachments + desc.colorAttachmentCount);
				m_desc.raster = desc.raster;
				m_desc.depthStencil = desc.depthStencil;
				m_desc.colorAttachments = m_colorAttachments.empty() ? nullptr : m_colorAttachments.data();
				m_desc.colorAttachmentCount = desc.colorAttachmentCount;
				m_desc.layout = GetLayout();
			}

			[[nodiscard]] const RHI::GraphicsPipelineDesc& GetDesc() const { return m_desc; }

		private:
			RHI::GraphicsPipelineDesc m_desc = {};
			std::vector<RHI::VertexBufferLayout> m_vertexBuffers;
			std::vector<RHI::VertexAttribute> m_vertexAttributes;
			std::vector<RHI::ColorAttachmentDesc> m_colorAttachments;
		};





		class NullResourceSet final : public RHI::ResourceSet
		{
		public:
			explicit NullResourceSet(const RHI::ResourceSetDesc& desc)
				: RHI::ResourceSet(desc)
			{
			}
		};

		class NullCommandList final : public RHI::ICommandList
		{
		public:
            // This backend applies submitted operations synchronously in queue order.
            void GlobalBarrierNative() override {}
            void CopyBufferNative(RHI::BufferHandle source,uint32_t,RHI::BufferHandle destination,uint32_t,uint32_t) override
            {
                if(m_closed || !m_valid || m_rendering ||
                    !dynamic_cast<NullBuffer*>(source) || !dynamic_cast<NullBuffer*>(destination))Invalidate();
            }
			void ResourceBarrierNative(
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
						barrier.after == RHI::ResourceState::Undefined)
					{
						Invalidate();
						continue;
					}

					if(barrier.buffer != nullptr)
					{
						auto* buffer = dynamic_cast<NullBuffer*>(barrier.buffer);
						if(buffer == nullptr || !NullValidation::IsBufferStateAllowed(buffer->GetDesc(), barrier.after))
						{
							Invalidate();
							continue;
						}
						Operation operation = {};
						operation.kind = OperationKind::BufferBarrier;
						operation.buffer = buffer;
						operation.after = barrier.after;
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
							!NullValidation::IsTextureStateAllowed(texture->GetDesc(), barrier.after) ||
							((barrier.before == RHI::ResourceState::Present ||
								barrier.after == RHI::ResourceState::Present) &&
								!texture->IsSwapchainImage()) ||
							!NullValidation::ResolveSubresources(
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
								Operation operation = {};
								operation.kind = OperationKind::TextureBarrier;
								operation.texture = texture;
								operation.after = barrier.after;
								operation.mipLevel = mip;
								operation.arrayLayer = layer;
								m_operations.push_back(std::move(operation));
							}
						}
					}
				}
			}

			void BeginRenderingNative(const RHI::RenderingDesc& desc) override
			{
				if(m_closed || m_rendering ||
					(desc.colorAttachmentCount != 0 && desc.colorAttachments == nullptr) ||
					(desc.colorAttachmentCount == 0 && desc.depthStencilAttachment == nullptr))
				{
					Invalidate();
					return;
				}
				m_pipeline = nullptr;
				for(uint32_t index = 0; index < desc.colorAttachmentCount; ++index)
				{
					const RHI::ColorAttachment& attachment = desc.colorAttachments[index];
					auto* texture = dynamic_cast<NullTexture*>(attachment.texture);
					const bool clearValueValid = attachment.loadOp != RHI::LoadOp::Clear ||
						std::all_of(std::begin(attachment.clearColor), std::end(attachment.clearColor),
							[](float component) { return std::isfinite(component); });
					if(texture == nullptr || !NullValidation::HasUsage(texture->GetDesc().usage, RHI::TextureUsage::RenderTarget) ||
						attachment.mipLevel >= texture->GetDesc().mipLevels ||
						attachment.arrayLayer >= texture->GetDesc().depthOrArraySize ||
						attachment.loadOp == RHI::LoadOp::Undefined || !clearValueValid ||
						attachment.storeOp == RHI::StoreOp::Undefined)
					{
						Invalidate();
						return;
					}
					TrackSwapchainImage(texture);
				}

				if(desc.depthStencilAttachment != nullptr)
				{
					const RHI::DepthStencilAttachment& attachment = *desc.depthStencilAttachment;
					auto* texture = dynamic_cast<NullTexture*>(attachment.texture);
					const bool subresourceValid = texture != nullptr &&
						attachment.mipLevel < texture->GetDesc().mipLevels &&
						attachment.arrayLayer < texture->GetDesc().depthOrArraySize;
					const bool hasStencil = texture != nullptr &&
						RHI::HasStencil(texture->GetDesc().format);
					const bool declaredStateValid =
						attachment.state == RHI::ResourceState::DepthRead ||
						attachment.state == RHI::ResourceState::DepthWrite;
					const bool requiresWrite = attachment.depthLoadOp == RHI::LoadOp::Clear ||
						(hasStencil && attachment.stencilLoadOp == RHI::LoadOp::Clear);
					const bool stateValid = subresourceValid && declaredStateValid &&
						(!requiresWrite || attachment.state == RHI::ResourceState::DepthWrite);
					const bool depthClearValid = attachment.depthLoadOp != RHI::LoadOp::Clear ||
						(std::isfinite(attachment.clearDepth) && attachment.clearDepth >= 0.0f &&
							attachment.clearDepth <= 1.0f);
					const bool stencilClearValid = attachment.stencilLoadOp != RHI::LoadOp::Clear ||
						attachment.clearStencil <= std::numeric_limits<uint8_t>::max();
					if(texture == nullptr || !NullValidation::HasUsage(texture->GetDesc().usage, RHI::TextureUsage::DepthStencil) ||
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
					}
				}

				m_rendering = true;
			}

			void EndRenderingNative() override
			{
				if(m_closed || !m_rendering)
				{
					Invalidate();
					return;
				}
				m_rendering = false;
			}

			void BindGraphicsPipelineNative(RHI::PipelineHandle pipelineState) override
			{
				auto* pipeline = dynamic_cast<NullPipelineState*>(pipelineState);
				if(m_closed || !m_rendering || pipeline == nullptr)
				{
					Invalidate();
					return;
				}
				m_pipeline = pipeline;
			}

			void BindResourceSetNative(RHI::ResourceSetHandle resourceSet) override
			{
				auto* set = dynamic_cast<NullResourceSet*>(resourceSet);
				if(m_closed || !m_rendering || m_pipeline == nullptr ||
					set == nullptr)
				{
					Invalidate();
					return;
				}
				for(const auto& requirement : set->GetStateRequirements())
				{
					if(requirement.texture)
						TrackSwapchainImage(static_cast<NullTexture*>(requirement.texture));
				}
			}

			void BindVertexBufferNative(
				uint32_t,
				RHI::BufferHandle buffer,
				uint32_t) override
			{
				auto* nullBuffer = dynamic_cast<NullBuffer*>(buffer);
				if(m_closed || !m_valid || !m_rendering || nullBuffer == nullptr)
				{
					Invalidate();
					return;
				}
			}

			void BindIndexBufferNative(
				RHI::BufferHandle buffer,
				RHI::Format,
				uint32_t) override
			{
				auto* nullBuffer = dynamic_cast<NullBuffer*>(buffer);
				if(m_closed || !m_valid || !m_rendering || nullBuffer == nullptr)
				{
					Invalidate();
					return;
				}
			}

			void SetInlineConstantsNative(
				uint32_t,
				uint32_t,
				const void*) override
			{
				if(m_closed || !m_rendering || m_pipeline == nullptr)
				{
					Invalidate();
					return;
				}
			}

			void SetViewportNative(const RHI::Viewport&) override
			{
				if(m_closed || !m_valid || !m_rendering)
				{
					Invalidate();
					return;
				}
			}

			void SetScissorNative(const RHI::Rect&) override
			{
				if(m_closed || !m_valid || !m_rendering)
				{
					Invalidate();
					return;
				}
			}

			void SetStencilReferenceNative(uint32_t) override
			{
				if(m_closed || !m_valid || !m_rendering)
				{
					Invalidate();
					return;
				}
			}

			void DrawInstancedNative(
				uint32_t,
				uint32_t,
				uint32_t,
				uint32_t) override
			{
				if(m_closed || !m_valid || !m_rendering)
				{
					Invalidate();
				}
			}

			void DrawIndexedInstancedNative(
				uint32_t,
				uint32_t,
				uint32_t,
				int32_t,
				uint32_t) override
			{
				if(m_closed || !m_valid || !m_rendering)
				{
					Invalidate();
				}
			}

			void DispatchMeshNative(uint32_t, uint32_t, uint32_t) override
			{
				if(m_closed || !m_valid || !m_rendering) Invalidate();
			}

            void DrawIndirectNative(RHI::BufferHandle arguments,uint32_t) override
            {
                auto* buffer=dynamic_cast<NullBuffer*>(arguments);
                if(m_closed || !m_valid || !m_rendering || !buffer)Invalidate();
            }

            void DrawIndexedIndirectNative(RHI::BufferHandle arguments,uint32_t,uint32_t,uint32_t) override
            {
                auto* buffer=dynamic_cast<NullBuffer*>(arguments);
                if(m_closed || !m_valid || !m_rendering || !buffer)Invalidate();
            }

			bool CloseNative() override
			{
				if(m_closed || m_rendering) Invalidate();
				m_closed = true;

        return m_valid;
    }

			[[nodiscard]] bool RecordBufferUpdate(
				NullBuffer* buffer,
				uint32_t offset,
				const void* data,
				uint32_t size)
			{
				if(m_closed || m_rendering)
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
				if(m_closed || m_rendering || texture->IsSwapchainImage())
				{
					return false;
				}
				if(NullValidation::FormatSize(texture->GetDesc().format) == 0)
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
				return true;
			}

			[[nodiscard]] bool IsClosed() const { return m_closed; }
			[[nodiscard]] bool IsValid() const { return m_valid; }
			[[nodiscard]] const std::vector<NullTexture*>& GetReferencedSwapchainImages() const
			{
				return m_referencedSwapchainImages;
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
					case OperationKind::TextureWrite:
						// Texture uploads are validated; Null has no pixel readback.
						break;
					case OperationKind::BufferWrite:
						operation.buffer->Write(operation.offset, operation.data);
						break;
					}
				}
			}

		private:
			enum class OperationKind : uint8_t
			{
				BufferBarrier,
				TextureBarrier,
				BufferWrite,
				TextureWrite
			};

			struct Operation
			{
				OperationKind kind = OperationKind::BufferBarrier;
				NullBuffer* buffer = nullptr;
				NullTexture* texture = nullptr;
				RHI::ResourceState after = RHI::ResourceState::Undefined;
				uint32_t offset = 0;
				uint32_t mipLevel = 0;
				uint32_t arrayLayer = 0;
				uint32_t rowPitch = 0;
				uint32_t slicePitch = 0;
				std::vector<uint8_t> data;
			};

			void Invalidate() { m_valid = false; }

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

			std::vector<Operation> m_operations;
			std::vector<NullTexture*> m_referencedSwapchainImages;
			NullPipelineState* m_pipeline = nullptr;
			bool m_rendering = false;
			bool m_closed = false;
			bool m_valid = true;
		};

	}

	struct NullDevice::Impl
	{
		std::vector<std::unique_ptr<NullTexture>> backBuffers;
		std::vector<uint64_t> imageCompletionValues;
		std::vector<uint64_t> frames;
		std::vector<std::unique_ptr<NullCommandList>> activeCommandLists;
		std::vector<std::unique_ptr<NullBuffer>> liveBuffers;
		std::vector<std::unique_ptr<NullTexture>> liveTextures;
		std::vector<std::unique_ptr<NullShader>> liveShaders;
		std::vector<std::unique_ptr<NullPipelineState>> livePipelines;
		std::vector<std::unique_ptr<NullResourceSet>> liveResourceSets;
		uint64_t nextCompletionValue = 1;
		uint64_t completedValue = 0;
		uint32_t nextFrameIndex = 0;
		uint32_t activeFrameIndex = 0;
		uint32_t activeImageIndex = 0;
		uint32_t nextImageIndex = 0;
		bool initialized = false;
		bool swapchainReady = false;
		bool frameReady = false;

	};

	NullDevice::NullDevice()
		: m_impl(new Impl())
	{
	}

	NullDevice::~NullDevice()
	{
        StopLiveProfiling();
        ReleaseResources();
		m_impl->activeCommandLists.clear();
		delete m_impl;
	}


uint64_t NullDevice::GetLastSubmissionNative() const {return m_impl->completedValue;}
uint64_t NullDevice::GetCompletedSubmissionNative() {return m_impl->completedValue;}
void NullDevice::DiscardCommandListNative(RHI::ICommandList* list) {auto& active=m_impl->activeCommandLists; active.erase(std::remove_if(active.begin(),active.end(),[list](const auto& value){return value.get()==list;}),active.end());}

bool NullDevice::SupportsNative(RHI::Feature feature) const
{
    // 실행 기능은 제공하지 않으며, 아래 설정 값은 손실 없이 기록하고 검증한다.
    switch(feature)
    {
    case RHI::Feature::SamplerLodBias:
    case RHI::Feature::FractionalDepthBias:
    case RHI::Feature::Wireframe:
    case RHI::Feature::DepthBiasClamp:
    case RHI::Feature::Tessellation:
    case RHI::Feature::IndirectDraw:
    case RHI::Feature::IndirectFirstInstance:
    case RHI::Feature::IndirectTessellation:
    case RHI::Feature::MeshShader: return true;
    case RHI::Feature::TaskShader: return false;
    default: return false;
    }
}
uint64_t NullDevice::GetLimitNative(RHI::Limit limit) const
{
    // 네이티브 GPU의 제약이 없으므로 RHI 입력 자료형의 범위를 그대로 제공한다.
    switch(limit)
    {
    case RHI::Limit::InlineConstantBytes:
    case RHI::Limit::Texture2DDimension:
    case RHI::Limit::TextureArrayLayers:
    case RHI::Limit::UniformBufferBytes:
    case RHI::Limit::StorageBufferBytes:
    case RHI::Limit::TessellationPatchControlPoints:
    case RHI::Limit::SamplerAnisotropy: return UINT32_MAX;
    case RHI::Limit::UniformBufferOffsetAlignment:
    case RHI::Limit::StorageBufferOffsetAlignment: return 1;
    default: return 0;
    }
}
bool NullDevice::SupportsSamplerNative(const RHI::SamplerDesc&) const
{return true;}
bool NullDevice::SupportsPipelineLayoutNative(const RHI::PipelineLayoutDesc&) const
{return true;}
bool NullDevice::SupportsGraphicsPipelineNative(const RHI::GraphicsPipelineDesc&) const
{
    return m_impl->initialized;
}
bool NullDevice::SupportsMeshPipelineNative(const RHI::MeshPipelineDesc&) const
{
    return m_impl && m_impl->initialized;
}
bool NullDevice::IsLostNative() const {return false;}
bool NullDevice::WaitIdleNative() { return m_impl->initialized; }
void NullDevice::DestroySwapchainNative()
{
    m_impl->backBuffers.clear();
    m_impl->imageCompletionValues.clear();
    m_impl->swapchainReady = m_impl->frameReady = false;
}

    bool NullDevice::CreateSwapchainNative(const RHI::SwapchainDesc& desc)
	{
		if(!m_impl->initialized || m_impl->swapchainReady ||
			desc.minimumImageCount == 0 || desc.initialWidth == 0 || desc.initialHeight == 0)
		{
			return false;
		}

		RHI::Format format = desc.format;
		switch(desc.presentMode)
		{
		case RHI::PresentMode::Immediate:
		case RHI::PresentMode::Mailbox:
		case RHI::PresentMode::Fifo:
			break;
		default:
			return false;
		}
		if(!NullValidation::IsColorFormat(format)) return false;
        if(desc.colorSpace > RHI::ColorSpace::LinearSrgb ||
            desc.compositeAlpha > RHI::CompositeAlpha::Inherit) return false;

		RHI::TextureDesc backBufferDesc = {};
		backBufferDesc.width = desc.initialWidth;
		backBufferDesc.height = desc.initialHeight;
		backBufferDesc.format = format;
		backBufferDesc.usage = RHI::TextureUsage::RenderTarget;

		std::vector<std::unique_ptr<NullTexture>> backBuffers;
		backBuffers.reserve(desc.minimumImageCount);
		for(uint32_t index = 0; index < desc.minimumImageCount; ++index)
		{
			backBuffers.push_back(std::make_unique<NullTexture>(backBufferDesc, true));
		}

		m_impl->backBuffers = std::move(backBuffers);
		m_impl->imageCompletionValues.assign(desc.minimumImageCount, 0);
		m_impl->nextImageIndex = 0;
		m_impl->swapchainReady = true;
		return true;
	}

	bool NullDevice::BeginFrameNative()
	{
		if(!m_impl->swapchainReady ||
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
		if(m_impl->frames[frameIndex] > m_impl->completedValue ||
			m_impl->imageCompletionValues[imageIndex] > m_impl->completedValue)
		{
			return false;
		}

		m_impl->activeFrameIndex = frameIndex;
		m_impl->activeImageIndex = imageIndex;
		m_impl->frameReady = true;
		return true;
	}

	RHI::ICommandList* NullDevice::AcquireCommandListNative()
	{
		if(!m_impl->initialized) return nullptr;
		auto commandList = std::make_unique<NullCommandList>();
		NullCommandList* result = commandList.get();
		m_impl->activeCommandLists.push_back(std::move(commandList));
		return result;
	}

	bool NullDevice::SubmitNative(RHI::ICommandList** cmdLists, uint32_t count)
	{
		if(cmdLists == nullptr || count == 0)
			return false;

		NullTexture* activeBackBuffer = nullptr;
		if(m_impl->frameReady && m_impl->activeImageIndex < m_impl->backBuffers.size())
			activeBackBuffer = m_impl->backBuffers[m_impl->activeImageIndex].get();

		std::vector<NullCommandList*> submitted;
		submitted.reserve(count);
		for(uint32_t index = 0; index < count; ++index)
            submitted.push_back(static_cast<NullCommandList*>(cmdLists[index]));

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
            if(owned == m_impl->activeCommandLists.end() || !(*owned)->IsClosed()) return false;
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

		for(NullCommandList* commandList : submitted) commandList->Apply();
		const uint64_t completionValue = m_impl->nextCompletionValue++;
		m_impl->completedValue = completionValue;
		if(m_impl->frameReady)
			m_impl->frames[m_impl->activeFrameIndex] = completionValue;
		if(frameSubmission)
			m_impl->imageCompletionValues[m_impl->activeImageIndex] = completionValue;
		return true;
	}

	bool NullDevice::PresentNative()
	{
		if(!m_impl->frameReady) return false;
        // 제출 횟수와 관계없이 Present가 프레임을 마감한다. 최종 상태 검사는 RHI가 담당한다.
		m_impl->nextFrameIndex = (m_impl->activeFrameIndex + 1) %
			static_cast<uint32_t>(m_impl->frames.size());
		m_impl->nextImageIndex = (m_impl->activeImageIndex + 1) %
			static_cast<uint32_t>(m_impl->backBuffers.size());
		m_impl->frameReady = false;
		return true;
	}

	RHI::BufferHandle NullDevice::CreateBufferNative(const RHI::BufferDesc& desc)
	{
		return m_impl->liveBuffers.emplace_back(
			std::make_unique<NullBuffer>(desc)).get();
	}

	RHI::TextureHandle NullDevice::CreateTextureNative(const RHI::TextureDesc& desc)
	{
		if((NullValidation::IsDepthFormat(desc.format) && NullValidation::HasUsage(desc.usage, RHI::TextureUsage::RenderTarget)) ||
			(!NullValidation::IsDepthFormat(desc.format) && NullValidation::HasUsage(desc.usage, RHI::TextureUsage::DepthStencil)))
		{
			return nullptr;
		}
		return m_impl->liveTextures.emplace_back(
			std::make_unique<NullTexture>(desc)).get();
	}

	RHI::ShaderHandle NullDevice::CreateShaderNative(const RHI::ShaderDesc& desc)
	{
		return m_impl->liveShaders.emplace_back(
			std::make_unique<NullShader>(desc)).get();
	}

	RHI::PipelineHandle NullDevice::CreateGraphicsPipelineNative(
		const RHI::GraphicsPipelineDesc& desc)
	{
		return m_impl->livePipelines.emplace_back(
			std::make_unique<NullPipelineState>(desc)).get();
	}

	RHI::PipelineHandle NullDevice::CreateMeshPipelineNative(
		const RHI::MeshPipelineDesc& desc)
	{
		auto pipeline = std::make_unique<NullPipelineState>(desc);
		NullPipelineState* result = pipeline.get();
		m_impl->livePipelines.push_back(std::move(pipeline));
		return result;
	}

	RHI::ResourceSetHandle NullDevice::CreateResourceSetNative(const RHI::ResourceSetDesc& desc)
	{
		return m_impl->liveResourceSets.emplace_back(
			std::make_unique<NullResourceSet>(desc)).get();
	}

	void NullDevice::DestroyBufferNative(RHI::BufferHandle buffer)
	{
        auto& objects = m_impl->liveBuffers;
        objects.erase(std::remove_if(objects.begin(), objects.end(),
            [buffer](const auto& object) { return object.get() == buffer; }), objects.end());
    }

	void NullDevice::DestroyTextureNative(RHI::TextureHandle texture)
	{
        auto& objects = m_impl->liveTextures;
        objects.erase(std::remove_if(objects.begin(), objects.end(),
            [texture](const auto& object) { return object.get() == texture; }), objects.end());
    }

	void NullDevice::DestroyShaderNative(RHI::ShaderHandle shader)
	{
        auto& objects = m_impl->liveShaders;
        objects.erase(std::remove_if(objects.begin(), objects.end(),
            [shader](const auto& object) { return object.get() == shader; }), objects.end());
    }

	void NullDevice::DestroyPipelineNative(RHI::PipelineHandle pipeline)
	{
        auto& objects = m_impl->livePipelines;
        objects.erase(std::remove_if(objects.begin(), objects.end(),
            [pipeline](const auto& object) { return object.get() == pipeline; }), objects.end());
    }

	void NullDevice::DestroyResourceSetNative(RHI::ResourceSetHandle resourceSet)
	{
        auto& objects = m_impl->liveResourceSets;
        objects.erase(std::remove_if(objects.begin(), objects.end(),
            [resourceSet](const auto& object) { return object.get() == resourceSet; }), objects.end());
    }

	bool NullDevice::UpdateBufferNative(
		RHI::ICommandList& commandList,
		RHI::BufferHandle buffer,
		uint32_t offset,
		const void* data,
		uint32_t size)
	{
        auto* nativeCommandList=dynamic_cast<NullCommandList*>(&commandList);
        auto* nativeBuffer=dynamic_cast<NullBuffer*>(buffer);
        return nativeCommandList && nativeBuffer &&
            nativeCommandList->RecordBufferUpdate(nativeBuffer, offset, data, size);
	}

	bool NullDevice::UpdateTextureNative(
		RHI::ICommandList& commandList,
		RHI::TextureHandle texture,
		uint32_t mipLevel,
		uint32_t arrayLayer,
		const void* data,
		uint32_t dataSize,
		uint32_t rowPitch,
		uint32_t slicePitch)
	{
        auto* nativeCommandList=dynamic_cast<NullCommandList*>(&commandList);
        auto* nativeTexture=dynamic_cast<NullTexture*>(texture);
        return nativeCommandList && nativeTexture &&
            nativeCommandList->RecordTextureUpdate(
                nativeTexture,
				mipLevel,
				arrayLayer,
				data,
				dataSize,
				rowPitch,
				slicePitch);
	}

	RHI::TextureHandle NullDevice::GetBackBufferNative()
	{
		if(!m_impl->swapchainReady || m_impl->backBuffers.empty())
			return nullptr;
		const uint32_t imageIndex = m_impl->frameReady
			? m_impl->activeImageIndex : m_impl->nextImageIndex;
		return imageIndex < m_impl->backBuffers.size()
			? m_impl->backBuffers[imageIndex].get() : nullptr;
	}

	int NullDevice::Initialize(const void*, const RHI::DeviceDesc& desc)
	{
		if(desc.maxFramesInFlight == 0 || desc.adapterIndex!=0)
			return -1;
		m_impl->frames.resize(desc.maxFramesInFlight);
		m_impl->initialized = true;
		return 0;
	}
}
