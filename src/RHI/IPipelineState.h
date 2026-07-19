#pragma once
#include <cstdint>
#include "Format.h"
#include "IShader.h"

namespace dy::RHI
{
	struct GraphicsPipelineDesc
	{
		// 참조한 shader는 이 pipeline보다 오래 살아야 한다.
		IShader* vertexShader = nullptr;
		IShader* fragmentShader = nullptr;

		Format renderTargetFormat = Format::Unknown;
		Format depthStencilFormat = Format::Unknown;

		bool depthEnable = false;
		bool wireframe = false;
		bool enableBindlessTextures = false;

		int32_t depthBias = 0;
		float depthBiasSlope = 0.0f;
		float depthBiasClamp = 0.0f;
	};

	class IPipelineState
	{
	public:
		virtual ~IPipelineState() = default;
	};
}
