#pragma once

namespace dy::Platform
{
	// Optional in-application RenderDoc bridge. It never starts RenderDoc or a
	// background service; it only talks to an API that was already injected into
	// this process by RenderDoc.
	class RenderDocCapture final
	{
	public:
		[[nodiscard]] static bool Initialize();
		[[nodiscard]] static bool IsAvailable();
		[[nodiscard]] static bool TriggerNextFrame();
	};
}
