#pragma once

namespace dy::Graphics
{
	enum class RenderPassKind
	{
		Shadow,
		MainForward
	};

	enum class RenderPassWork
	{
		PrepareOnly,
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
