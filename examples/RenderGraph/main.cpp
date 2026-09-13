#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <thread>
#include <mutex>
#include <set>

#include "RHI/ICommandList.h"
#include "RHI/IDevice.h"
#include "Graphics/RenderGraph.h"
#include "Core/ThreadPool.h"

// 멀티 플랫폼 정석 3대 리소스 배리어 방출 검증용 테스트 CommandList
class TestLoggingCommandList final : public dy::RHI::ICommandList
{
public:
	explicit TestLoggingCommandList(uint32_t id = 0) : m_id(id) {}

	// 3대 리소스 배리어 방출 가로채기 및 로깅
	void TextureBarrier(dy::RHI::ITexture*, uint32_t beforeAccess, uint32_t afterAccess) override
	{
		std::lock_guard<std::mutex> lock(s_printMutex);
		std::cout << "     [Barrier Emitted / CmdList " << m_id << "] -> 1. Texture Barrier (Access: " << beforeAccess << " -> " << afterAccess << ")\n";
		textureBarrierCount++;
	}

	void BufferBarrier(dy::RHI::IBuffer*, uint32_t beforeAccess, uint32_t afterAccess) override
	{
		std::lock_guard<std::mutex> lock(s_printMutex);
		std::cout << "     [Barrier Emitted / CmdList " << m_id << "] -> 2. Buffer Barrier (Access: " << beforeAccess << " -> " << afterAccess << ")\n";
		bufferBarrierCount++;
	}

	void GlobalBarrier(uint32_t beforeAccess, uint32_t afterAccess) override
	{
		std::lock_guard<std::mutex> lock(s_printMutex);
		std::cout << "     [Barrier Emitted / CmdList " << m_id << "] -> 3. Global Barrier (Access: " << beforeAccess << " -> " << afterAccess << ")\n";
		globalBarrierCount++;
	}

	// 나머지 RHI 필수 순수 가상 함수 구현부 (더미)
	void BindGraphicsPipeline(dy::RHI::IPipelineState*) override {}
	void BindGlobalDescriptors() override {}
	void BindGeometry(const dy::RHI::GeometryBinding&) override {}
	void BindVertexBuffer(dy::RHI::IBuffer*, uint32_t, uint32_t) override {}
	void BindIndexBuffer(dy::RHI::IBuffer*, dy::RHI::Format, uint32_t) override {}
	void SetInlineConstants(uint32_t, const void*) override {}
	void SetRenderTargets(uint32_t, dy::RHI::ITexture**, dy::RHI::ITexture*) override {}
	void SetViewport(const dy::RHI::Viewport&) override {}
	void SetScissor(const dy::RHI::Rect&) override {}
	void ClearColor(dy::RHI::ITexture*, float, float, float, float) override {}
	void ClearDepth(dy::RHI::ITexture*, float) override {}
	void DrawInstanced(uint32_t, uint32_t, uint32_t, uint32_t) override {}
	void DrawIndexedInstanced(uint32_t, uint32_t, uint32_t, int32_t, uint32_t) override {}
	void Close() override {}

	uint32_t m_id = 0;
	uint32_t textureBarrierCount = 0;
	uint32_t bufferBarrierCount = 0;
	uint32_t globalBarrierCount = 0;

	static std::mutex s_printMutex;
};

std::mutex TestLoggingCommandList::s_printMutex;

// 멀티스레드 커맨드리스트 제출 검증용 테스트 Device
class TestLoggingDevice final : public dy::RHI::IDevice
{
public:
	TestLoggingDevice()
	{
		m_mainCmdList = std::make_unique<TestLoggingCommandList>(0);
		for (uint32_t i = 0; i < 8; ++i)
		{
			m_workerCmdLists.push_back(std::make_unique<TestLoggingCommandList>(i + 1));
		}
	}

	void BeginFrame() override {}
	uint32_t GetCurrentFrameIndex() const override { return 0; }

	dy::RHI::ICommandList* AcquireCommandList() override
	{
		return m_mainCmdList.get();
	}

	dy::RHI::ICommandList* AcquireWorkerCommandList(uint32_t threadIndex) override
	{
		if (threadIndex < m_workerCmdLists.size())
		{
			return m_workerCmdLists[threadIndex].get();
		}
		return m_mainCmdList.get();
	}

	void ResetCommandLists() override {}

	void Submit(dy::RHI::ICommandList** cmdLists, uint32_t count) override
	{
		std::lock_guard<std::mutex> lock(TestLoggingCommandList::s_printMutex);
		totalSubmittedCommandLists += count;
		std::cout << "  [Device Submit] Successfully submitted " << count << " CommandLists to GPU Queue.\n";
		for (uint32_t i = 0; i < count; ++i)
		{
			auto* testCmd = static_cast<TestLoggingCommandList*>(cmdLists[i]);
			uint32_t cmdId = testCmd ? testCmd->m_id : 0;
			submittedCmdIds.push_back(cmdId);
			std::cout << "    - Batch item " << (i + 1) << ": CommandList ID " << cmdId << "\n";
		}
	}

	void Present() override {}

	dy::RHI::IBuffer* CreateBuffer(const dy::RHI::BufferDesc&) override { return nullptr; }
	dy::RHI::ITexture* CreateTexture(const dy::RHI::TextureDesc&) override { return nullptr; }
	dy::RHI::IPipelineState* CreateGraphicsPipeline(const dy::RHI::GraphicsPipelineDesc&) override { return nullptr; }
	dy::RHI::ITexture* GetBackBuffer() override { return nullptr; }

	void DestroyBuffer(dy::RHI::IBuffer*) override {}
	void DestroyTexture(dy::RHI::ITexture*) override {}
	void DestroyPipelineState(dy::RHI::IPipelineState*) override {}
	bool UpdateTexture(dy::RHI::ITexture*, const void*, uint32_t) override { return true; }

	int Initialize(const void*, const dy::RHI::DeviceDesc&) override { return 0; }

	uint32_t totalSubmittedCommandLists = 0;
	std::vector<uint32_t> submittedCmdIds;

private:
	std::unique_ptr<TestLoggingCommandList> m_mainCmdList;
	std::vector<std::unique_ptr<TestLoggingCommandList>> m_workerCmdLists;
};

int main()
{
	std::cout << "========================================================\n";
	std::cout << "  dy_engine: Multi-threaded RenderGraph Stage Test      \n";
	std::cout << "========================================================\n\n";

	using namespace dy::Graphics;

	// ----------------------------------------------------------------------------------
	// [Part 1] 기존 단일 스레드 위상 정렬 및 3대 배리어 방출 검증
	// ----------------------------------------------------------------------------------
	std::cout << ">>> [PART 1] Verifying Single-Thread Execution & 3 Barrier Emissions <<<\n";
	{
		RenderGraph graph;
		RGResourceHandle shadowMap = graph.ImportTexture("ShadowMap", nullptr);
		RGResourceHandle backBuffer = graph.ImportTexture("BackBuffer", nullptr);
		RGResourceHandle screenOutput = graph.ImportTexture("ScreenOutput", nullptr);
		RGResourceHandle sceneUniforms = graph.ImportBuffer("SceneUniforms", nullptr);

		graph.AddPass("PostProcessingPass")
			.Read(backBuffer, RGResourceAccess::ShaderRead)
			.Write(screenOutput, RGResourceAccess::RenderTarget)
			.TextureBarrier(backBuffer, RGResourceAccess::RenderTarget, RGResourceAccess::ShaderRead)
			.SetExecute([](dy::RHI::ICommandList*) {
				std::cout << "  -> Executing [PostProcessingPass]\n";
			});

		graph.AddPass("MainForwardPass")
			.Read(shadowMap, RGResourceAccess::ShaderRead)
			.Read(sceneUniforms, RGResourceAccess::ShaderRead)
			.Write(backBuffer, RGResourceAccess::RenderTarget)
			.TextureBarrier(shadowMap, RGResourceAccess::DepthWrite, RGResourceAccess::ShaderRead)
			.BufferBarrier(sceneUniforms, RGResourceAccess::CopyDst, RGResourceAccess::ShaderRead)
			.SetExecute([](dy::RHI::ICommandList*) {
				std::cout << "  -> Executing [MainForwardPass]\n";
			});

		graph.AddPass("ShadowPass")
			.Write(shadowMap, RGResourceAccess::DepthWrite)
			.GlobalBarrier(RGResourceAccess::Undefined, RGResourceAccess::DepthWrite)
			.SetExecute([](dy::RHI::ICommandList*) {
				std::cout << "  -> Executing [ShadowPass]\n";
			});

		bool compileOk = graph.Compile();
		assert(compileOk);
		(void)compileOk;

		TestLoggingCommandList loggingCmdList(0);
		graph.Execute(&loggingCmdList);

		assert(loggingCmdList.textureBarrierCount == 2);
		assert(loggingCmdList.bufferBarrierCount == 1);
		assert(loggingCmdList.globalBarrierCount == 1);
		std::cout << "  [PART 1 PASSED] Single-thread & 3-tier barrier tests successfully verified!\n\n";
	}

	// ----------------------------------------------------------------------------------
	// [Part 2] 멀티스레드 병렬 실행 (Kahn Stage 분할 + ThreadPool 병렬 녹화) 검증
	// ----------------------------------------------------------------------------------
	std::cout << ">>> [PART 2] Verifying Multi-Threaded Parallel Pass Recording <<<\n";
	{
		RenderGraph graph;

		// 1. 리소스 선언
		RGResourceHandle skinData = graph.ImportBuffer("SkinData", nullptr);
		RGResourceHandle shadowMap = graph.ImportTexture("ShadowMap", nullptr);
		RGResourceHandle backBuffer = graph.ImportTexture("BackBuffer", nullptr);
		RGResourceHandle screenOutput = graph.ImportTexture("ScreenOutput", nullptr);

		std::cout << "[1] Registering passes with parallel candidate branches:\n";
		std::cout << "    - Pass A: ComputeSkinningPass (Writes SkinData)     -> Independent (Stage 0)\n";
		std::cout << "    - Pass B: ShadowPass          (Writes ShadowMap)    -> Independent (Stage 0)\n";
		std::cout << "    - Pass C: MainForwardPass     (Reads A & B, Writes BackBuffer) -> Depends on Stage 0 (Stage 1)\n";
		std::cout << "    - Pass D: ToneMapPass         (Reads BackBuffer, Writes ScreenOutput) -> Depends on Stage 1 (Stage 2)\n\n";

		std::set<std::thread::id> stage0ThreadIds;
		std::mutex stage0Mutex;

		// Pass A: Compute Skinning (독립 패스)
		graph.AddPass("ComputeSkinningPass")
			.Write(skinData, RGResourceAccess::UnorderedAccess)
			.BufferBarrier(skinData, RGResourceAccess::Undefined, RGResourceAccess::UnorderedAccess)
			.SetExecute([&stage0ThreadIds, &stage0Mutex](dy::RHI::ICommandList*) {
				auto tid = std::this_thread::get_id();
				{
					std::lock_guard<std::mutex> lock(stage0Mutex);
					stage0ThreadIds.insert(tid);
				}
				std::lock_guard<std::mutex> lock(TestLoggingCommandList::s_printMutex);
				std::cout << "  [Thread " << tid << "] Recording [ComputeSkinningPass]\n";
			});

		// Pass B: Shadow Pass (독립 패스)
		graph.AddPass("ShadowPass")
			.Write(shadowMap, RGResourceAccess::DepthWrite)
			.GlobalBarrier(RGResourceAccess::Undefined, RGResourceAccess::DepthWrite)
			.SetExecute([&stage0ThreadIds, &stage0Mutex](dy::RHI::ICommandList*) {
				auto tid = std::this_thread::get_id();
				{
					std::lock_guard<std::mutex> lock(stage0Mutex);
					stage0ThreadIds.insert(tid);
				}
				std::lock_guard<std::mutex> lock(TestLoggingCommandList::s_printMutex);
				std::cout << "  [Thread " << tid << "] Recording [ShadowPass]\n";
			});

		// Pass C: Main Forward (Pass A와 Pass B 결과를 모두 읽음)
		graph.AddPass("MainForwardPass")
			.Read(skinData, RGResourceAccess::ShaderRead)
			.Read(shadowMap, RGResourceAccess::ShaderRead)
			.Write(backBuffer, RGResourceAccess::RenderTarget)
			.TextureBarrier(shadowMap, RGResourceAccess::DepthWrite, RGResourceAccess::ShaderRead)
			.SetExecute([](dy::RHI::ICommandList*) {
				std::lock_guard<std::mutex> lock(TestLoggingCommandList::s_printMutex);
				std::cout << "  [Thread " << std::this_thread::get_id() << "] Recording [MainForwardPass]\n";
			});

		// Pass D: ToneMap (Pass C 결과를 읽음)
		graph.AddPass("ToneMapPass")
			.Read(backBuffer, RGResourceAccess::ShaderRead)
			.Write(screenOutput, RGResourceAccess::RenderTarget)
			.TextureBarrier(backBuffer, RGResourceAccess::RenderTarget, RGResourceAccess::ShaderRead)
			.SetExecute([](dy::RHI::ICommandList*) {
				std::lock_guard<std::mutex> lock(TestLoggingCommandList::s_printMutex);
				std::cout << "  [Thread " << std::this_thread::get_id() << "] Recording [ToneMapPass]\n";
			});

		// 2. 컴파일 및 Stage 분할 검증
		std::cout << "[2] Compiling RenderGraph & Analyzing Dependency Stages:\n";
		bool compileSuccess = graph.Compile();
		assert(compileSuccess);
		if (!compileSuccess) return -1;

		const auto& stages = graph.GetExecutionStages();
		std::cout << "  [SUCCESS] Graph compiled into " << stages.size() << " Dependency Stages:\n";
		for (size_t s = 0; s < stages.size(); ++s)
		{
			std::cout << "    Stage " << s << " (" << stages[s].passIndices.size() << " passes): ";
			for (uint32_t pIdx : stages[s].passIndices)
			{
				std::cout << "[" << graph.GetPass(pIdx)->GetName() << "] ";
			}
			std::cout << (stages[s].passIndices.size() > 1 ? "(PARALLEL CANDIDATE)" : "(SERIAL)") << "\n";
		}
		std::cout << "\n";

		// 검증: Stage 0에는 의존성이 없는 2개의 패스(ComputeSkinningPass, ShadowPass)가 있어야 함!
		assert(stages.size() == 3);
		assert(stages[0].passIndices.size() == 2);
		assert(stages[1].passIndices.size() == 1);
		assert(stages[2].passIndices.size() == 1);

		// 3. ThreadPool 및 Device 초기화
		std::cout << "[3] Initializing ThreadPool (4 worker threads) & Test Device:\n";
		dy::Core::ThreadPool threadPool(4);
		TestLoggingDevice testDevice;
		std::cout << "  ThreadPool initialized with " << threadPool.GetThreadCount() << " worker threads.\n\n";

		// 4. 멀티스레드 병렬 실행
		std::cout << "[4] Executing RenderGraph in Parallel Mode (ExecuteParallel):\n";
		graph.ExecuteParallel(&testDevice, &threadPool);
		std::cout << "\n";

		// 5. 검증
		std::cout << "[5] Verifying Parallel Execution Results:\n";
		std::cout << "    - Dependency Stages Executed: " << stages.size() << "\n";
		std::cout << "    - Total CommandLists Submitted: " << testDevice.totalSubmittedCommandLists << " (Expected: 4)\n";

		assert(testDevice.totalSubmittedCommandLists == 4);
		std::set<uint32_t> uniqueCmdIds(testDevice.submittedCmdIds.begin(), testDevice.submittedCmdIds.end());
		assert(uniqueCmdIds.size() == testDevice.totalSubmittedCommandLists);

		std::cout << "  [ALL MULTI-THREADING TESTS PASSED] Passes were successfully recorded and submitted!\n";
	}

	std::cout << "\n========================================================\n";
	std::cout << "  Multi-threaded RenderGraph Test Completed!  \n";
	std::cout << "========================================================\n";

	return 0;
}
