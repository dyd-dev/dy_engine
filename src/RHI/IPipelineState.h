#pragma once
#include <cstddef>
#include <cstdint>
#include "Format.h"
#include "ShaderLayout.h"

namespace dy::RHI
{
	inline constexpr uint32_t kDefaultShadowMapResolution = 2048u;

	struct GraphicsPipelineDesc
	{
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
		bool enableBindlessTextures = false;
		uint32_t shadowMapResolution = kDefaultShadowMapResolution;

		// 깊이 전용(그림자) 파이프라인용 래스터라이저 깊이 바이어스.
		// pixelShader == nullptr 인 깊이 전용 PSO 에서 그림자 아크네 완화에 사용.
		int32_t depthBias = 0;
		float depthBiasSlope = 0.0f;
		float depthBiasClamp = 0.0f;
	};

	struct ComputePipelineDesc
	{
		const void* computeShader = nullptr;
		size_t computeShaderSize = 0;
	};

	class IPipelineState
	{
	public:
		virtual ~IPipelineState() = default;
	};
}
