#include <android_native_app_glue.h>
#include "dyf/Platform/Window.h"

#if defined(DY_ACTIVE_SAMPLE_SHADOW)
extern int RunShadowMapping();
#else
extern int RunPBRLighting();
#endif

void android_main(struct android_app* state)
{
    dyf::Platform::Window::SetAndroidApp(state);

#if defined(DY_ACTIVE_SAMPLE_SHADOW)
    RunShadowMapping();
#else
    RunPBRLighting();
#endif
}
