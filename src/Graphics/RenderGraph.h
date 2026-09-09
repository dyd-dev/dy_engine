#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>

namespace dy::RHI
{
	class ITexture;
	class IBuffer;
	class IPipelineState;
	class ICommandList;
}

namespace dy::Graphics
{
	// 백엔드 중립적인 리소스 접근 상태
	enum class RGResourceAccess
	{
		Undefined,
		ShaderRead,
		RenderTarget,
		DepthWrite,
		DepthRead,
		UnorderedAccess,
		CopySrc,
		CopyDst,
		Present
	};

	// 렌더 그래프 리소스 핸들 (가상 리소스 식별자)
	struct RGResourceHandle
	{
		uint32_t id = 0xFFFFFFFFu;

		bool IsValid() const { return id != 0xFFFFFFFFu; }
		bool operator==(const RGResourceHandle& other) const { return id == other.id; }
		bool operator!=(const RGResourceHandle& other) const { return id != other.id; }
	};

	struct RGResourceBinding
	{
		RGResourceHandle handle;
		RGResourceAccess access = RGResourceAccess::Undefined;
	};

	enum class RGResourceType
	{
		Texture,
		Buffer
	};

	struct RGResourceDesc
	{
		std::string name;
		RGResourceType type = RGResourceType::Texture;
		RHI::ITexture* texturePtr = nullptr;
		RHI::IBuffer* bufferPtr = nullptr;
	};

	enum class RGBarrierType
	{
		Texture, // 1. 텍스처 배리어 (Texture / Image Barrier)
		Buffer,  // 2. 버퍼 배리어 (Buffer Memory Barrier)
		Global   // 3. 글로벌 메모리 배리어 (Global / Pipeline Memory Barrier)
	};

	struct RGBarrierCmd
	{
		RGBarrierType type = RGBarrierType::Texture;
		RGResourceHandle resourceHandle;
		RGResourceAccess beforeAccess = RGResourceAccess::Undefined;
		RGResourceAccess afterAccess = RGResourceAccess::Undefined;
	};

	using RGPassExecuteCallback = std::function<void(RHI::ICommandList* cmdList)>;

	class RenderGraphPass
	{
	public:
		explicit RenderGraphPass(std::string name, uint32_t index)
			: m_name(std::move(name)), m_index(index) {}

		RenderGraphPass& Read(RGResourceHandle resource, RGResourceAccess access);
		RenderGraphPass& Write(RGResourceHandle resource, RGResourceAccess access);
		RenderGraphPass& SetPipeline(RHI::IPipelineState* pipeline);
		RenderGraphPass& SetExecute(RGPassExecuteCallback callback);

		// 멀티 플랫폼 정석 3대 리소스 배리어 API
		// 1. Texture Barrier (텍스처 상태/레이아웃 전환 배리어)
		RenderGraphPass& TextureBarrier(RGResourceHandle texture, RGResourceAccess beforeAccess, RGResourceAccess afterAccess);
		// 2. Buffer Barrier (버퍼 접근 권한/캐시 동기화 배리어)
		RenderGraphPass& BufferBarrier(RGResourceHandle buffer, RGResourceAccess beforeAccess, RGResourceAccess afterAccess);
		// 3. Global Barrier (전역 메모리/파이프라인 캐시 플러시 배리어)
		RenderGraphPass& GlobalBarrier(RGResourceAccess beforeAccess, RGResourceAccess afterAccess);

		[[nodiscard]] const std::string& GetName() const { return m_name; }
		[[nodiscard]] uint32_t GetIndex() const { return m_index; }
		[[nodiscard]] uint64_t GetRevision() const { return m_revision; }
		[[nodiscard]] RHI::IPipelineState* GetPipeline() const { return m_pipeline; }
		[[nodiscard]] const std::vector<RGResourceBinding>& GetReads() const { return m_reads; }
		[[nodiscard]] const std::vector<RGResourceBinding>& GetWrites() const { return m_writes; }
		[[nodiscard]] const std::vector<RGBarrierCmd>& GetBarriers() const { return m_barriers; }
		[[nodiscard]] bool HasExecuteCallback() const { return static_cast<bool>(m_executeCallback); }

		void Execute(RHI::ICommandList* cmdList) const;

	private:
		std::string m_name;
		uint32_t m_index = 0;
		uint64_t m_revision = 0;
		RHI::IPipelineState* m_pipeline = nullptr;
		std::vector<RGResourceBinding> m_reads;
		std::vector<RGResourceBinding> m_writes;
		std::vector<RGBarrierCmd> m_barriers;
		RGPassExecuteCallback m_executeCallback;
	};

	class RenderGraph
	{
	public:
		RenderGraph() = default;
		~RenderGraph() = default;

		RenderGraph(const RenderGraph&) = delete;
		RenderGraph& operator=(const RenderGraph&) = delete;
		RenderGraph(RenderGraph&&) = default;
		RenderGraph& operator=(RenderGraph&&) = default;

		// 1. 외부 RHI 리소스 가져오기 (Import)
		RGResourceHandle ImportTexture(const std::string& name, RHI::ITexture* texture);
		RGResourceHandle ImportBuffer(const std::string& name, RHI::IBuffer* buffer);

		// 2. Pass 추가
		RenderGraphPass& AddPass(const std::string& name);

		// 3. 그래프 컴파일 (리소스 의존성 분석 + 위상 정렬 Topological Sort)
		bool Compile();

		// 4. 단일 스레드 실행
		void Execute(RHI::ICommandList* commandList);

		// 5. 초기화 / 재사용
		void Reset();

		// 디버깅 및 정보 조회 API
		[[nodiscard]] bool IsCompiled() const;
		[[nodiscard]] const std::vector<uint32_t>& GetExecutionOrderIndices() const { return m_executionOrder; }
		[[nodiscard]] std::vector<std::string> GetExecutionOrderNames() const;
		[[nodiscard]] const RenderGraphPass* GetPass(uint32_t index) const;
		[[nodiscard]] const RGResourceDesc* GetResourceDesc(RGResourceHandle handle) const;

	private:
		std::vector<RGResourceDesc> m_resources;
		std::unordered_map<std::string, RGResourceHandle> m_resourceNameToHandle;

		std::vector<std::unique_ptr<RenderGraphPass>> m_passes;
		std::vector<uint32_t> m_executionOrder; // 위상 정렬된 패스 인덱스 순서
		std::vector<uint64_t> m_compiledRevisions;
		bool m_compiled = false;
	};
}
