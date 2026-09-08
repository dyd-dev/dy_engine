#pragma once

#include "Graphics/RendererConfig.h"

namespace dy::Graphics
{
	struct SkinningExecutionDecision
	{
		SkinningExecutionMode active = SkinningExecutionMode::VertexShader;
		bool fellBack = false;
	};

	[[nodiscard]] SkinningExecutionDecision ResolveSkinningExecutionMode(
		SkinningExecutionMode requested,
		bool computeSkinningSupported);
}
