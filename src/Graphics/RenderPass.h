#pragma once

namespace dy::Graphics
{
	enum class RenderPassKind
	{
		Skinning,
		Shadow,
		MainForward
	};

	enum class RenderPassWork
	{
		PrepareOnly,
		Compute,
		Graphics
	};

	struct RenderPassDesc
	{
		RenderPassKind kind = RenderPassKind::MainForward;
		RenderPassWork work = RenderPassWork::Graphics;
		const char* name = "MainForward";
		bool enabled = true;
	};
}
