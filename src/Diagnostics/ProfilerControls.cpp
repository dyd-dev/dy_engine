#include "dyf/Diagnostics/ProfilerControls.h"
#include "dyf/Platform/Input.h"
#include "dyf/Platform/RenderDocCapture.h"
#include "dyf/Renderer.h"

namespace dyf::Diagnostics
{
bool UpdateProfilerControls(const Platform::Input& input,Renderer* renderer)
{
    if(input.WasPressed(Platform::Key::F12))
        (void)Platform::RenderDocCapture::TriggerNextFrame();
    if(renderer&&input.WasPressed(Platform::Key::F11))
    {
        auto config=renderer->GetConfig();
        config.profilerStartsExpanded=!config.profilerStartsExpanded;
        return renderer->SetConfig(config);
    }
    return true;
}
}
