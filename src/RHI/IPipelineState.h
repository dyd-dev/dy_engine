#pragma once
#include "Enums.h"
#include <cstddef>
#include <cstdint>

namespace dy::RHI
{
	struct GraphicsPipelineDesc {
		const void* vertexShader = nullptr;
		size_t vertexShaderSize = 0;

		const void* pixelShader = nullptr;
		size_t pixelShaderSize = 0;

		const void* shadowVertexShader = nullptr;
		size_t shadowVertexShaderSize = 0;

		Format renderTargetFormat = Format::Unknown;
		Format depthStencilFormat = Format::Unknown;

		// flags like Depth, Blend mode
		bool depthEnable = true;
		bool wireframe = false;
		bool enableShadowPass = false;
		uint32_t shadowMapResolution = 2048;
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
