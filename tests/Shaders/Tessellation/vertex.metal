#include <metal_stdlib>
using namespace metal;

vertex float4 vertexMain(uint vertexID [[vertex_id]])
{
    constexpr float2 positions[] = {
        float2(-0.8f, -0.8f),
        float2( 0.8f, -0.8f),
        float2( 0.0f,  0.8f)
    };
    return float4(positions[vertexID], 0.0f, 1.0f);
}
