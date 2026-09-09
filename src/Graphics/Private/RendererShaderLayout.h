#pragma once

#include "Graphics/ShaderLayout.h"

namespace dy::Graphics::Private::RendererShaderLayout
{
	using namespace dy::Graphics::ShaderLayout;
	struct RendererVertex
	{
		float px = 0.0f;
		float py = 0.0f;
		float pz = 0.0f;
		float nx = 0.0f;
		float ny = 0.0f;
		float nz = 1.0f;
		float u = 0.0f;
		float v = 0.0f;
		float tx = 1.0f;
		float ty = 0.0f;
		float tz = 0.0f;
		float tw = 1.0f;
	};
}
