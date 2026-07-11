#include "Graphics/SkinningPass.h"

namespace dy::Graphics
{
	SkinningExecutionDecision ResolveSkinningExecutionMode(
		SkinningExecutionMode requested,
		bool computeSkinningSupported)
	{
		if(requested == SkinningExecutionMode::ComputePreSkin && computeSkinningSupported)
			return SkinningExecutionDecision{ SkinningExecutionMode::ComputePreSkin, false };
		if(requested == SkinningExecutionMode::ComputePreSkin)
			return SkinningExecutionDecision{ SkinningExecutionMode::VertexShader, true };
		return SkinningExecutionDecision{ SkinningExecutionMode::VertexShader, false };
	}
}
