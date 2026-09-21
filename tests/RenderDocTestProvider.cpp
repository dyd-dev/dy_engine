#include <renderdoc_app.h>
#if defined(_WIN32)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif
namespace
{
int captures=0;
void RENDERDOC_CC Trigger() { ++captures; }
RENDERDOC_API_1_1_2 api{};
}
extern "C" EXPORT int RENDERDOC_CC RENDERDOC_GetAPI(RENDERDOC_Version version,void** result)
{
    if(version!=eRENDERDOC_API_Version_1_1_2 || !result) return 0;
    api.TriggerCapture=Trigger;
    *result=&api;
    return 1;
}
extern "C" EXPORT int CaptureCount() { return captures; }
