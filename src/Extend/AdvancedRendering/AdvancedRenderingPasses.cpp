#include "dyf/Extends/AdvancedRendering.h"

namespace dyf
{
bool AdvancedRendering::AddPasses(RenderStage stage,RenderFrame& frame,RHI::RenderGraph& graph)
{
    for(auto& module:m_modules)
        if(!module->AddPasses(stage,frame,graph)) return false;
    return true;
}
}
