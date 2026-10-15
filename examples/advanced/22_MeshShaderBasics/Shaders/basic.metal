#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float4 color;
};

using TriangleMeshType = metal::mesh<VertexOut, void, 3, 1, metal::topology::triangle>;

[[mesh, max_total_threads_per_threadgroup(3)]]
void meshMain(
    TriangleMeshType output,
    uint tid [[thread_index_in_threadgroup]])
{
    const float2 positions[3] = {
        float2( 0.0f,  0.5f),
        float2( 0.5f, -0.5f),
        float2(-0.5f, -0.5f)
    };

    const float3 colors[3] = {
        float3(1.0f, 0.0f, 0.0f),
        float3(0.0f, 1.0f, 0.0f),
        float3(0.0f, 0.0f, 1.0f)
    };

    if (tid < 3)
    {
        output.set_vertex(tid, VertexOut{ float4(positions[tid], 0.0f, 1.0f), float4(colors[tid], 1.0f) });
    }

    if (tid == 0)
    {
        output.set_primitive_count(1);
        output.set_index(0, 0);
        output.set_index(1, 1);
        output.set_index(2, 2);
    }
}

fragment float4 fragmentMain(VertexOut in [[stage_in]]) {
    return in.color;
}
