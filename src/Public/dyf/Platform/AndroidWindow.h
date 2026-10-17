#pragma once
struct android_app;
namespace dyf::Platform
{
// The NativeActivity owns app and its ANativeWindow. Set before creating Window;
// clear only after every Window and RHI device using that activity has been destroyed.
void SetAndroidApp(android_app*) noexcept;
[[nodiscard]] android_app* GetAndroidApp() noexcept;
}
