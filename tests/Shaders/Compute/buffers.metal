#include <metal_stdlib>
using namespace metal;

kernel void computeMain(uint3 id [[thread_position_in_grid]],
    device float4* values [[buffer(0)]], const device float4* seed [[buffer(1)]],
    constant uint& factor [[buffer(2)]], constant uint& pass [[buffer(15)]])
{
    const uint index = id.x + 8 * id.y + 32 * id.z; // 8 x 4 x 4 grid.
    values[index] = pass == 0 ? seed[0] + float4(float(index + 1) / 256, 0, 0, 0) : values[index] * factor;
}

vertex float4 vertexMain(uint id [[vertex_id]])
{
    return float4(float2((id << 1) & 2, id & 2) * 2 - 1, 0, 1);
}

fragment float4 fragmentMain(float4 position [[position]], const device float4* values [[buffer(0)]])
{
    return values[uint(position.x) + 8 * uint(position.y)];
}
