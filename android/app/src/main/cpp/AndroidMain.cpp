#include <android_native_app_glue.h>
#include "dyf/Platform/Window.h"
#include "Example.h"
#include <exception>
#include <cstdio>

void android_main(struct android_app* state)
{
    dyf::Platform::Window::SetAndroidApp(state);
    try
    {
        dyf::Platform::Window window(960,640,"Advanced Android");
        char name[]="AdvancedAndroid";
        char* arguments[]={name,nullptr};
#if defined(DY_ACTIVE_SAMPLE_SHADOW)
        auto example=advanced::CreateShadowMapping(window.GetHandle(),1,arguments);
#else
        auto example=advanced::CreatePBRLighting(window.GetHandle(),1,arguments);
#endif
        if(!example) return;
        while(window.IsRunning())
        {
            window.PollEvents();
            if(!window.IsRunning() || !example->Tick())break;
        }
        example->WaitIdle();
    }
    catch(const std::exception& error)
    {
        std::fprintf(stderr,"Advanced Android: %s\n",error.what());
    }
    dyf::Platform::Window::SetAndroidApp(nullptr);
}
