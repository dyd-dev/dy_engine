#include <dyf/RHI.h>
#include <functional>
#include <utility>

// buffer는 Vertex 용도로 생성되어 실제로 CopyDestination 상태여야 한다.
// upload는 UpdateBuffer를 기록하고 실패 시 예외 등으로 호출자에게 전달한다.
// draw는 자신의 rendering 구간과 attachment, pipeline, 바인딩을 설정한다.
// callback에서 buffer의 상태를 직접 바꾸지 않는다.
bool ConfigureGraph(
    dyf::RHI::RenderGraph& graph,
    dyf::RHI::BufferHandle buffer,
    std::function<void(dyf::RHI::ICommandList*)> upload,
    std::function<void(dyf::RHI::ICommandList*)> draw)
{
    using namespace dyf::RHI;
    if (!buffer || !upload || !draw) return false;

    graph.Reset();
    const auto data = graph.ImportBuffer("mesh-data", buffer,
        ResourceState::CopyDestination, ResourceState::VertexBuffer);
    if (!data.IsValid()) return false;

    // 등록 순서가 같은 자원의 데이터 의존성을 결정한다.
    graph.AddPass("Upload")
        .Write(data, ResourceState::CopyDestination)
        .SetExecute(std::move(upload));
    graph.AddPass("Draw")
        .Read(data, ResourceState::VertexBuffer)
        .SetExecute(std::move(draw));

    // 실제 실행 순서는 Upload -> Draw이며 그래프가 배리어를 기록한다.
    // buffer의 수명은 적어도 Execute 기록이 끝날 때까지 유지한다.
    // 호출자는 Compile 성공 후 Execute, Close, Submit의 반환값을 확인한다.
    // Execute가 실패한 목록은 Reset/Destroy하고, callback 예외도 처리한다.
    // 재실행 전에는 실제 buffer 상태를 다시 CopyDestination으로 맞춘다.
    return graph.Compile();
}