#include <RHI/RenderGraph.h>
#include <utility>

// Callbacks record their own barriers and commands; buffer stays caller-owned.
bool ConfigureGraph(
    dy::RHI::RenderGraph& graph,
    dy::RHI::BufferHandle buffer,
    dy::RHI::RGPassExecuteCallback upload,
    dy::RHI::RGPassExecuteCallback draw)
{
    using namespace dy::RHI;
    graph.Reset();
    const RGResourceHandle data = graph.ImportBuffer("mesh-data", buffer);

    // Declare the consumer first to demonstrate dependency ordering.
    graph.AddPass("Draw")
        .Read(data, RGResourceAccess::ShaderRead)
        .SetExecute(std::move(draw));
    graph.AddPass("Upload")
        .Write(data, RGResourceAccess::CopyDst)
        .SetExecute(std::move(upload));

    // On success, the order is Upload -> Draw.
    // The caller executes the graph, closes the command list, then submits it.
    return graph.Compile();
}
