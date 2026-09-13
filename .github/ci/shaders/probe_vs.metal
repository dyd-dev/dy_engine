#include <metal_stdlib>
using namespace metal;
struct Constants { float4 color; float4 parameters; };
struct Output { float4 position [[position]]; float2 uv; };
vertex Output main0(uint id [[vertex_id]], device const float* vertices [[buffer(4)]],
                    constant Constants& constants [[buffer(0)]])
{
    uint offset = id * 12;
    Output output;
    output.position = float4(vertices[offset] * constants.parameters.x,
                             vertices[offset + 1] * constants.parameters.x * constants.parameters.z,
                             constants.parameters.y, 1);
    output.uv = float2(vertices[offset + 6], vertices[offset + 7]);
    return output;
}
