#pragma once
#include <cstdint>
#include <memory>

#include "Format.h"
#include "ShaderLayout.h"

namespace dy::RHI
{
	class ICommandList;

	class IBuffer;
	class ITexture;
	class IPipelineState;

	struct BufferDesc;
	struct TextureDesc;
	struct GraphicsPipelineDesc;
	struct ComputePipelineDesc;

	struct DeviceDesc
	{
		// 스왑체인(백버퍼) 포맷. 백엔드는 이 값을 그대로 따르고 GetBackBuffer()->GetFormat() 로 보고한다.
		// UNORM = 셰이더 수동 감마, *_SRGB = 하드웨어 감마. 두 백엔드가 같은 값을 쓰므로 색이 일치한다.
		Format swapchainFormat = Format::R8G8B8A8_UNORM;
		uint32_t maxFramesInFlight = 2;
		// 모든 기록 draw의 합계(그림자 뷰, 메인, 후처리 포함).
		uint32_t maxDrawsPerFrame = 128;
		uint32_t maxBindlessTextures = 128;
		uint64_t frameAcquireTimeoutNanoseconds = 16666667ull;
		ShaderLayoutDesc shaderLayout = {};
	};

	struct GpuTimestampResult
	{
		uint64_t durationNanoseconds = 0;
		uint64_t frameSerial = 0;
	};

	struct ResourceAllocationCounter
	{
		uint64_t live = 0;
		uint64_t created = 0;
		uint64_t destroyed = 0;
	};

	// Counts RHI GPU objects only. These are deliberately object counters, not
	// CPU heap hooks, requested bytes, native heap sizes, or residency metrics.
	struct ResourceAllocationCounters
	{
		ResourceAllocationCounter buffers = {};
		ResourceAllocationCounter textures = {};
		ResourceAllocationCounter pipelines = {};

		[[nodiscard]] uint64_t GetTotalLive() const
		{
			return buffers.live + textures.live + pipelines.live;
		}
	};

	class IDevice
	{
	public:
		IDevice();
		virtual ~IDevice();
		[[nodiscard]] static IDevice* Create(const void* windowHandle, const DeviceDesc& desc = {});
		[[nodiscard]] const DeviceDesc& GetDesc() const { return m_desc; }

		virtual void BeginFrame() = 0;

		[[nodiscard]] virtual uint32_t GetCurrentFrameIndex() const = 0;
		[[nodiscard]] virtual RHI::ICommandList* AcquireCommandList()	= 0;

		virtual void Submit(ICommandList** cmdLists, uint32_t count) = 0;
		virtual void Present() = 0;

		[[nodiscard]] virtual ITexture* GetBackBuffer() = 0;

		[[nodiscard]] virtual IBuffer* CreateBuffer(const BufferDesc& desc) = 0;
		[[nodiscard]] virtual ITexture* CreateTexture(const TextureDesc& desc) = 0;
		[[nodiscard]] virtual IPipelineState* CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) = 0;
		[[nodiscard]] virtual IPipelineState* CreateComputePipeline(const ComputePipelineDesc& desc)
		{
			(void)desc;
			return nullptr;
		}
		
		virtual void DestroyBuffer(IBuffer* buffer) = 0;
		virtual void DestroyTexture(ITexture* texture) = 0;
		virtual void DestroyPipelineState(IPipelineState* pipeline) = 0;

		virtual bool UpdateTexture(ITexture* texture, const void* data, uint32_t rowPitch) = 0;
		[[nodiscard]] ResourceAllocationCounters GetResourceAllocationCounters() const;

		// Timestamp results are intentionally delayed: true means a completed GPU
		// frame has published a value for this name, never an in-flight estimate.
		[[nodiscard]] virtual bool SupportsGpuTimestamps() const { return false; }
		[[nodiscard]] virtual uint32_t GetMaxGpuTimestampScopes() const { return 0; }
		[[nodiscard]] virtual bool TryGetLastGpuTimestamp(const char* name, GpuTimestampResult& result) const
		{
			(void)name;
			(void)result;
			return false;
		}

		// Graphics가 그림자 깊이 패스를 명시적으로 기록해야 하는 백엔드.
		[[nodiscard]] virtual bool RequiresExplicitShadowPass() const { return false; }
		// (D3D12/Metal: false. Vulkan: true)
		[[nodiscard]] virtual bool RequiresClipSpaceYFlip() const { return false; }
		[[nodiscard]] virtual bool SupportsShadowAtlas() const { return false; }
		// Renderer bindings 13/14 are currently provided by the Vulkan descriptor layout only.
		[[nodiscard]] virtual bool SupportsSkinningStorageBindings() const { return false; }
		[[nodiscard]] virtual bool SupportsComputeSkinning() const { return false; }

		[[nodiscard]] virtual DescriptorIndex AllocateDescriptorSlot() { return INVALID_DESCRIPTOR_INDEX; }
		virtual void UpdateDescriptorSlot(DescriptorIndex index, ITexture* texture) { (void)index; (void)texture; }
		virtual void UpdateDescriptorSlot(DescriptorIndex index, IBuffer* buffer) { (void)index; (void)buffer; }

	protected:
		virtual int Initialize(const void* windowHandle, const DeviceDesc& desc) = 0;
		bool TrackBufferCreated(IBuffer* buffer);
		bool TrackTextureCreated(ITexture* texture);
		bool TrackPipelineCreated(IPipelineState* pipeline);
		bool TrackBufferDestroyed(IBuffer* buffer);
		bool TrackTextureDestroyed(ITexture* texture);
		bool TrackPipelineDestroyed(IPipelineState* pipeline);

	private:
		enum class ResourceKind : uint8_t { Buffer, Texture, Pipeline };
		struct ResourceCounterState;
		bool TrackResourceCreated(ResourceKind kind, const void* resource);
		bool TrackResourceDestroyed(ResourceKind kind, const void* resource);
		void SetDesc(const DeviceDesc& desc) { m_desc = desc; }

		DeviceDesc m_desc = {};
		std::unique_ptr<ResourceCounterState> m_resourceCounterState;
	};
}
