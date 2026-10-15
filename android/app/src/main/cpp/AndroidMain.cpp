#include <android_native_app_glue.h>
#include "dyf/Platform/Window.h"
#include "dyf/Platform/Log.h"

#if defined(DY_ACTIVE_SAMPLE_SHADOW)
extern int RunShadowMapping();
#else
extern int RunPBRLighting();
#endif

void android_main(struct android_app* state)
{
    dyf::Platform::Log::Initialize();
    dyf::Platform::Log::Write(dyf::Platform::LogLevel::Info, "Android", "dy_engine android_main started");

    dyf::Platform::Window::SetAndroidApp(state);

#if defined(DY_ACTIVE_SAMPLE_SHADOW)
    dyf::Platform::Log::Write(dyf::Platform::LogLevel::Info, "Android", "Launching Sample: 08_ShadowMapping");
    RunShadowMapping();
#else
    dyf::Platform::Log::Write(dyf::Platform::LogLevel::Info, "Android", "Launching Sample: 07_PBRLighting");
    RunPBRLighting();
#endif

    dyf::Platform::Log::Write(dyf::Platform::LogLevel::Info, "Android", "dy_engine android_main finished");
}
