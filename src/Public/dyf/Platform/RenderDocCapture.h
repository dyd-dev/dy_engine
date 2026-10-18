#pragma once
#include <cstdint>

namespace dyf::Platform
{
	// Uses an already injected API. Only explicit ConnectReplayUI/OpenReplayUI calls launch its UI.
	class RenderDocCapture final
	{
	public:
		enum class ReplayUIResult : std::uint8_t { Unavailable,NoCaptures,AlreadyConnected,Launched,LaunchFailed };
		[[nodiscard]] static bool Initialize();
		[[nodiscard]] static bool IsAvailable();
		[[nodiscard]] static bool IsEnabled();
		[[nodiscard]] static bool IsReplayUIConnected();
		[[nodiscard]] static bool TriggerNextFrame();
		[[nodiscard]] static std::uint32_t GetCaptureCount();
		// Launched confirms a process was started; its target connection may still be pending.
		[[nodiscard]] static ReplayUIResult ConnectReplayUI();
		[[nodiscard]] static ReplayUIResult OpenReplayUI();
	};
}
