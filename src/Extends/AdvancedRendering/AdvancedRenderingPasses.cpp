#include "dyf/Extends/AdvancedRendering.h"

namespace dyf
{
bool AdvancedRendering::AddPasses(RenderStage stage,ReadOnlyRenderFrame& frame,RHI::RenderGraph& graph)
{
    for(auto& module:m_modules)
        if(!static_cast<ReadOnlyRenderExtension&>(*module).AddPasses(stage,frame,graph)) return false;
    return true;
}
}
