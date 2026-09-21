#include "dyf/Platform/RenderDocCapture.h"
#include <cstring>
#include <iostream>
#if defined(_WIN32)
#include <Windows.h>
#else
#include <dlfcn.h>
#endif
int main(int argc,char** argv)
{
    using dyf::Platform::RenderDocCapture;
    if(argc!=2) return 1;
    if(std::strcmp(argv[1],"--absent")==0)
        return RenderDocCapture::Initialize() || RenderDocCapture::IsAvailable() || RenderDocCapture::TriggerNextFrame() ? 1 : 0;
#if defined(_WIN32)
    auto module=LoadLibraryA(argv[1]);
    if(!module) return 2;
    auto count=reinterpret_cast<int(*)()>(GetProcAddress(module,"CaptureCount"));
#else
    auto module=dlopen(argv[1],RTLD_NOW|RTLD_GLOBAL);
    if(!module) return 2;
    auto count=reinterpret_cast<int(*)()>(dlsym(module,"CaptureCount"));
#endif
    if(!count || count()!=0 || !RenderDocCapture::Initialize() || !RenderDocCapture::IsAvailable()) return 3;
    if(!RenderDocCapture::TriggerNextFrame() || !RenderDocCapture::TriggerNextFrame() || count()!=2) return 4;
    std::cout << "Only the already loaded API was used; two requests reached TriggerCapture.\n";
    return 0;
}
