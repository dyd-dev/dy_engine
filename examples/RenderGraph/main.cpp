#include <iostream>
#include <vector>
#include <string>
#include <cassert>

#include "RHI/ICommandList.h"
#include "Graphics/RenderGraph.h"

// 멀티 플랫폼 정석 3대 리소스 배리어 방출 검증용 테스트 CommandList
class TestLoggingCommandList final : public dy::RHI::ICommandList
{
public:
	// 3대 리소스 배리어 방출 가로채기 및 로깅
	void TextureBarrier(dy::RHI::ITexture*, uint32_t beforeAccess, uint32_t afterAccess) override
	{
		std::cout << "     [Barrier Emitted] -> 1. Texture Barrier (Access: " << beforeAccess << " -> " << afterAccess << ")\n";
		textureBarrierCount++;
	}

	void BufferBarrier(dy::RHI::IBuffer*, uint32_t beforeAccess, uint32_t afterAccess) override
	{
		std::cout << "     [Barrier Emitted] -> 2. Buffer Barrier (Access: " << beforeAccess << " -> " << afterAccess << ")\n";
		bufferBarrierCount++;
	}

	void GlobalBarrier(uint32_t beforeAccess, uint32_t afterAccess) override
	{
		std::cout << "     [Barrier Emitted] -> 3. Global Barrier (Access: " << beforeAccess << " -> " << afterAccess << ")\n";
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

	uint32_t textureBarrierCount = 0;
	uint32_t bufferBarrierCount = 0;
	uint32_t globalBarrierCount = 0;
};

int main()
{
	std::cout << "========================================================\n";
	std::cout << "  dy_engine: Single-Thread RenderGraph Barrier Test     \n";
	std::cout << "========================================================\n\n";

	using namespace dy::Graphics;

	RenderGraph graph;

	// 1. 리소스 선언 (가상 핸들)
	RGResourceHandle shadowMap = graph.ImportTexture("ShadowMap", nullptr);
	RGResourceHandle backBuffer = graph.ImportTexture("BackBuffer", nullptr);
	RGResourceHandle screenOutput = graph.ImportTexture("ScreenOutput", nullptr);
	RGResourceHandle sceneUniforms = graph.ImportBuffer("SceneUniforms", nullptr);

	std::cout << "[1] Registering Passes in Out-of-Order sequence...\n";
	std::cout << "    - Registering Pass 1: PostProcessingPass (Reads BackBuffer, Writes ScreenOutput)\n";
	std::cout << "    - Registering Pass 2: MainForwardPass   (Reads ShadowMap & SceneUniforms, Writes BackBuffer)\n";
	std::cout << "    - Registering Pass 3: ShadowPass        (Writes ShadowMap)\n\n";

	// 2. 의도적으로 거꾸로/순서가 뒤섞이게 Pass 등록
	// Pass 1: PostProcessing
	graph.AddPass("PostProcessingPass")
		.Read(backBuffer, RGResourceAccess::ShaderRead)
		.Write(screenOutput, RGResourceAccess::RenderTarget)
		.TextureBarrier(backBuffer, RGResourceAccess::RenderTarget, RGResourceAccess::ShaderRead) // [API 1] Texture Barrier
		.SetExecute([](dy::RHI::ICommandList*) {
			std::cout << "  -> Executing [PostProcessingPass]\n";
		});

	// Pass 2: Main Forward
	graph.AddPass("MainForwardPass")
		.Read(shadowMap, RGResourceAccess::ShaderRead)
		.Read(sceneUniforms, RGResourceAccess::ShaderRead)
		.Write(backBuffer, RGResourceAccess::RenderTarget)
		.TextureBarrier(shadowMap, RGResourceAccess::DepthWrite, RGResourceAccess::ShaderRead) // [API 1] Texture Barrier
		.BufferBarrier(sceneUniforms, RGResourceAccess::CopyDst, RGResourceAccess::ShaderRead) // [API 2] Buffer Barrier
		.SetExecute([](dy::RHI::ICommandList*) {
			std::cout << "  -> Executing [MainForwardPass]\n";
		});

	// Pass 3: Shadow Pass
	graph.AddPass("ShadowPass")
		.Write(shadowMap, RGResourceAccess::DepthWrite)
		.GlobalBarrier(RGResourceAccess::Undefined, RGResourceAccess::DepthWrite)              // [API 3] Global Barrier
		.SetExecute([](dy::RHI::ICommandList*) {
			std::cout << "  -> Executing [ShadowPass]\n";
		});

	// 3. 렌더그래프 컴파일 (위상 정렬)
	std::cout << "[2] Compiling RenderGraph (Topological Sort / Dependency Analysis)...\n";
	bool compileSuccess = graph.Compile();

	if (!compileSuccess)
	{
		std::cerr << "  [ERROR] Failed to compile RenderGraph! Cycle detected or invalid dependencies.\n";
		return -1;
	}
	std::cout << "  [SUCCESS] RenderGraph Compiled Successfully!\n\n";

	// 4. 컴파일 결과 순서 확인
	std::cout << "[3] Resulting Execution Order:\n";
	auto orderNames = graph.GetExecutionOrderNames();
	for (std::size_t i = 0; i < orderNames.size(); ++i)
	{
		std::cout << "    Step " << (i + 1) << ": " << orderNames[i] << "\n";
	}
	std::cout << "\n";

	// 5. 검증 (ShadowPass -> MainForwardPass -> PostProcessingPass 순서이어야 함)
	std::vector<std::string> expectedOrder = { "ShadowPass", "MainForwardPass", "PostProcessingPass" };
	assert(orderNames == expectedOrder);
	if (orderNames == expectedOrder)
	{
		std::cout << "  [VERIFICATION PASSED] Execution order matches expected dependency graph!\n\n";
	}
	else
	{
		std::cerr << "  [VERIFICATION FAILED] Execution order mismatch!\n\n";
		return -1;
	}

	// 6. 단일 스레드 Execute() 및 3대 배리어 방출 테스트 수행
	std::cout << "[4] Executing RenderGraph passes & Emitting 3 Resource Barriers to CommandList:\n";
	TestLoggingCommandList loggingCmdList;
	graph.Execute(&loggingCmdList);

	// 7. 배리어 방출 카운트 검증
	std::cout << "\n[5] Verifying 3 Resource Barrier Emissions:\n";
	std::cout << "    - Texture Barriers Emitted: " << loggingCmdList.textureBarrierCount << " (Expected: 2)\n";
	std::cout << "    - Buffer Barriers Emitted:  " << loggingCmdList.bufferBarrierCount << " (Expected: 1)\n";
	std::cout << "    - Global Barriers Emitted:  " << loggingCmdList.globalBarrierCount << " (Expected: 1)\n\n";

	assert(loggingCmdList.textureBarrierCount == 2);
	assert(loggingCmdList.bufferBarrierCount == 1);
	assert(loggingCmdList.globalBarrierCount == 1);

	std::cout << "  [ALL BARRIER TESTS PASSED] 3 Resource Barrier APIs are successfully emitted to GPU!\n";

	std::cout << "\n========================================================\n";
	std::cout << "  RenderGraph Test Completed Successfully!  \n";
	std::cout << "========================================================\n";

	return 0;
}
