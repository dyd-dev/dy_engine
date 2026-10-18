#pragma once

struct android_app;

namespace dyf::Platform
{
	// Internal NativeActivity binding used by the packaged Android application.
	class AndroidApplication final
	{
	public:
		explicit AndroidApplication(android_app&) noexcept;
		~AndroidApplication();
		AndroidApplication(const AndroidApplication&) = delete;
		AndroidApplication& operator=(const AndroidApplication&) = delete;
	};
}
