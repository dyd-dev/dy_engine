#pragma once
#include <cstddef>
#include <cstdint>
#include "Format.h"
#include "ShaderLayout.h"

namespace dy::RHI
{
	struct GraphicsPipelineDesc
	{
		const void* vertexShader = nullptr;
		size_t vertexShaderSize = 0;

		const void* pixelShader = nullptr;
		size_t pixelShaderSize = 0;

		Format renderTargetFormat = Format::Unknown;
		Format depthStencilFormat = Format::Unknown;

		bool depthEnable = false;
		bool wireframe = false;
		bool enableBindlessTextures = false;

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
