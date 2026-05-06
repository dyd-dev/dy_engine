#pragma once
/* PipelineState.h
* 
* PipelineState는 그래픽스 파이프라인의 상태를 정의하는 클래스입니다.
* Shader, BlendState, RasterizerState, DepthStencilState 등과 같은 그래픽스 파이프라인의 다양한 상태를 캡슐화하여 관리합니다.
*/
#include "Enums.h"
#include <cstddef>

namespace dy::RHI
{
	struct GraphicsPipelineDesc {
		const void* vertexShader;
		size_t vertexShaderSize;

		const void* pixelShader;
		size_t pixelShaderSize;

		Format renderTargetFormat;
		Format depthStencilFormat;

		// flags like Depth, Blend mode
		bool depthEnable = true;
		bool wireframe = false;
	};

	struct ComputePipelineDesc {
		const void* computeShader;
		size_t computeShaderSize;
	};

	class IPipelineState
	{
	public:
		virtual ~IPipelineState() = default;
	};
}
