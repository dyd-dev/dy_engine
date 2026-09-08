#include <iostream>
#include <vector>
#include <string>
#include <cassert>

#include "Graphics/RenderGraph.h"

int main()
{
	std::cout << "========================================================\n";
	std::cout << "  dy_engine: Single-Thread RenderGraph Dependency Test  \n";
	std::cout << "========================================================\n\n";

	using namespace dy::Graphics;

	RenderGraph graph;

	// 1. 리소스 선언 (가상 핸들)
	RGResourceHandle shadowMap = graph.ImportTexture("ShadowMap", nullptr);
	RGResourceHandle backBuffer = graph.ImportTexture("BackBuffer", nullptr);
	RGResourceHandle screenOutput = graph.ImportTexture("ScreenOutput", nullptr);

	std::cout << "[1] Registering Passes in Out-of-Order sequence...\n";
	std::cout << "    - Registering Pass 1: PostProcessingPass (Reads BackBuffer, Writes ScreenOutput)\n";
	std::cout << "    - Registering Pass 2: MainForwardPass   (Reads ShadowMap, Writes BackBuffer)\n";
	std::cout << "    - Registering Pass 3: ShadowPass        (Writes ShadowMap)\n\n";

	// 2. 의도적으로 거꾸로/순서가 뒤섞이게 Pass 등록
	// Pass 1: PostProcessing
	graph.AddPass("PostProcessingPass")
		.Read(backBuffer, RGResourceAccess::ShaderRead)
		.Write(screenOutput, RGResourceAccess::RenderTarget)
		.SetExecute([](dy::RHI::ICommandList*) {
			std::cout << "  -> Executing [PostProcessingPass]\n";
		});

	// Pass 2: Main Forward
	graph.AddPass("MainForwardPass")
		.Read(shadowMap, RGResourceAccess::ShaderRead)
		.Write(backBuffer, RGResourceAccess::RenderTarget)
		.SetExecute([](dy::RHI::ICommandList*) {
			std::cout << "  -> Executing [MainForwardPass]\n";
		});

	// Pass 3: Shadow Pass
	graph.AddPass("ShadowPass")
		.Write(shadowMap, RGResourceAccess::DepthWrite)
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

	// 6. 단일 스레드 Execute() 수행
	std::cout << "[4] Executing RenderGraph passes in sorted order:\n";
	graph.Execute(nullptr);

	std::cout << "\n========================================================\n";
	std::cout << "  RenderGraph Test Completed Successfully!  \n";
	std::cout << "========================================================\n";

	return 0;
}
